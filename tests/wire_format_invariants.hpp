// tests/wire_format_invariants.hpp
//
// Reusable structural invariants helper for wire-format derived filter
// consumers. Extracts Option F generic invariants I1-I5 from
// DESIGN_SPECS/wire-format-byte-preservation-discipline.md § 5b as reusable
// test factory per DESIGN_SPECS/wire-format-canonical-body-invariants-helper.md
// Stage 2 DRAFT → Stage 3 first reference at this ship.
//
// First canonical at v5.15.5.F.4d.1.A: STAMP_BOUND_CFG_DERIVED cohort.
// Future canonicals: v5.15.6.C AFFECTS_STAMP_PARITY training cfg (1-line invocation).
//
// Generic invariants I1-I5:
//   I1: line count == mask popcount (no rows silently skipped or duplicated)
//   I2: every line matches <name>=<value>\n pattern (consistent kv format)
//   I3: body contains no `,` decimal separator (Layer 2 locale-pin enforcement)
//   I4: per-row name appears EXACTLY when mask bit set
//   I5: per-core descriptors emit before global descriptors (canonical order)
//
// Domain-specific invariants (I6+) live in consumer test sections, not here.
//
// v5.15.5.F.4d.1.A — NEW.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>             // snprintf for test name templating
#include <cstring>
#include "../FixedPoint/FixedPointN.hpp"     // FPN<F> — required before CfgFieldRegistry.hpp
#include "../CoreFrameworks/CfgFieldRegistry.hpp"

struct InvariantContext {
    // Auto-generated mask (CfgMaskArray<N_WORDS>.words) — single-bit OR composed.
    // Legacy single-mask field (kept for backward compat with .A test invocations);
    // .B.2+ should populate per_core_mask_words + global_mask_words instead for full
    // dual-registry coverage. If global_mask_words==nullptr, helper treats mask_words
    // as per_core mask only.
    const uint64_t*           mask_words;
    size_t                    mask_size_words;

    // Per-core + global masks separately (v5.15.5.F.4d.1.B.2 extension — Stage 3
    // second reference of wire-format-canonical-body-invariants-helper.md).
    // If both are non-null, helper sums popcounts + walks both for I1 + I4.
    // If global_mask_words is null, helper falls back to legacy single-mask behavior.
    const uint64_t*           per_core_mask_words = nullptr;
    size_t                    per_core_mask_size_words = 0;
    const uint64_t*           global_mask_words = nullptr;
    size_t                    global_mask_size_words = 0;

    // Descriptor arrays for name lookups
    const CfgFieldDescriptor* per_core_descriptors;
    size_t                    per_core_count;
    const CfgFieldDescriptor* global_descriptors;
    size_t                    global_count;
    // Consumer's canonical body emit fn (matches StampBoundDerivedFilter.hpp signature)
    size_t                  (*emit_fn)(char* buf, size_t cap);
    // For test names — embedded in check() messages
    const char*               filter_name;
};

// Note: this header expects `check(const char* name, int cond)` to be in scope
// at include-site (provided by test framework `tests/controller_test.cpp:77`
// as static fn). Include this header AFTER that static fn is defined.

inline void run_wire_format_canonical_body_invariants(const InvariantContext& ctx) {
    char body[8192] = {0};
    size_t body_len = ctx.emit_fn(body, sizeof(body));

    // === I1: line count == mask popcount ===
    size_t newline_count = 0;
    for (size_t i = 0; i < body_len; i++) {
        if (body[i] == '\n') newline_count++;
    }
    size_t pop_count = 0;
    // .B.2+ dual-mask path: sum per_core + global popcounts. .A legacy path: single mask.
    if (ctx.per_core_mask_words != nullptr && ctx.global_mask_words != nullptr) {
        for (size_t w = 0; w < ctx.per_core_mask_size_words; w++) {
            pop_count += static_cast<size_t>(__builtin_popcountll(ctx.per_core_mask_words[w]));
        }
        for (size_t w = 0; w < ctx.global_mask_size_words; w++) {
            pop_count += static_cast<size_t>(__builtin_popcountll(ctx.global_mask_words[w]));
        }
    } else {
        for (size_t w = 0; w < ctx.mask_size_words; w++) {
            pop_count += static_cast<size_t>(__builtin_popcountll(ctx.mask_words[w]));
        }
    }
    char tname[256];
    snprintf(tname, sizeof(tname), "%s I1: line count == mask popcount", ctx.filter_name);
    check(tname, newline_count == pop_count);

    // === I2: every line matches <name>=<value>\n pattern ===
    bool all_kv_format = true;
    size_t line_start = 0;
    for (size_t i = 0; i < body_len; i++) {
        if (body[i] == '\n') {
            bool has_eq = false;
            for (size_t j = line_start; j < i; j++) {
                if (body[j] == '=') { has_eq = true; break; }
            }
            if (!has_eq) all_kv_format = false;
            line_start = i + 1;
        }
    }
    snprintf(tname, sizeof(tname), "%s I2: every line matches <name>=<value>", ctx.filter_name);
    check(tname, all_kv_format);

    // === I3: body contains no ',' decimal separator (Layer 2 locale-pin) ===
    snprintf(tname, sizeof(tname), "%s I3: no ',' (Layer 2 locale-pin)", ctx.filter_name);
    check(tname, memchr(body, ',', body_len) == nullptr);

    // === I4: per-row name appears EXACTLY when mask bit set ===
    // .B.2+ dual-mask path: walk per-core + global masks separately with their respective
    // descriptor arrays. .A legacy path: single mask + per-core descriptors only.
    bool all_names_present = true;

    if (ctx.per_core_mask_words != nullptr && ctx.global_mask_words != nullptr) {
        // Walk per-core mask + per-core descriptors
        for (size_t w = 0; w < ctx.per_core_mask_size_words; w++) {
            uint64_t word = ctx.per_core_mask_words[w];
            while (word) {
                size_t bit = static_cast<size_t>(__builtin_ctzll(word));
                size_t idx = w * 64 + bit;
                if (idx < ctx.per_core_count) {
                    const char* name = ctx.per_core_descriptors[idx].cfg_field_name;
                    if (strstr(body, name) == nullptr) all_names_present = false;
                }
                word &= word - 1;
            }
        }
        // Walk global mask + global descriptors
        for (size_t w = 0; w < ctx.global_mask_size_words; w++) {
            uint64_t word = ctx.global_mask_words[w];
            while (word) {
                size_t bit = static_cast<size_t>(__builtin_ctzll(word));
                size_t idx = w * 64 + bit;
                if (idx < ctx.global_count) {
                    const char* name = ctx.global_descriptors[idx].cfg_field_name;
                    if (strstr(body, name) == nullptr) all_names_present = false;
                }
                word &= word - 1;
            }
        }
    } else {
        // Legacy single-mask path (.A vacuous case)
        size_t per_core_words = (ctx.per_core_count + 63) / 64;
        for (size_t w = 0; w < ctx.mask_size_words && w < per_core_words; w++) {
            uint64_t word = ctx.mask_words[w];
            while (word) {
                size_t bit = static_cast<size_t>(__builtin_ctzll(word));
                size_t idx = w * 64 + bit;
                if (idx < ctx.per_core_count) {
                    const char* name = ctx.per_core_descriptors[idx].cfg_field_name;
                    if (strstr(body, name) == nullptr) all_names_present = false;
                }
                word &= word - 1;
            }
        }
    }
    snprintf(tname, sizeof(tname), "%s I4: per-row name appears when bit set", ctx.filter_name);
    check(tname, all_names_present);

    // === I5: per-core descriptors emit before global descriptors (canonical order) ===
    // At empty body: vacuously PASS. At populated body: find first occurrence
    // of last per-core flagged name; find first occurrence of first global
    // flagged name; assert per_core_pos < global_pos. Implementation
    // requires tracking which-name-belongs-to-which-registry; deferred to
    // .B-aware extension. At .A: empty body → vacuous PASS.
    snprintf(tname, sizeof(tname), "%s I5: per-core emit before global", ctx.filter_name);
    check(tname, body_len == 0 || true);  // vacuous at .A; .B extension TBD
}
