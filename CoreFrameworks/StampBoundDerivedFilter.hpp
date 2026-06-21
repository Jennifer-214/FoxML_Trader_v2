// CoreFrameworks/StampBoundDerivedFilter.hpp
//
// First canonical consumer of FOREACH_METADATA_BIT auto-generated mask
// infrastructure for wire-format derived filters per
// DESIGN_SPECS/metadata-bit-driven-derived-filter-framework.md v1.2 Path γ
// correction (2026-05-17).
//
// Mechanism: consumes auto-generated g_*_cfg_stamp_bound_cfg_derived_mask
// (constexpr .rodata; X-macro generated from FOREACH_METADATA_BIT row added at
// CfgFieldRegistry.hpp:1075) via CFG_FIELD_FOR_EACH_SET_BIT (branchless TZCNT).
// Iteration order: per-core first, then global (canonical body order).
//
// At .A landing: zero rows have STAMP_BOUND_CFG_DERIVED bit; emit produces
// empty body; wire_format_invariants helper verifies vacuously. .B flags 24
// rows; same code produces populated body; same invariants verify shape.
//
// Layer 2 locale pin per ModelInference.hpp:1697 precedent — thread-local
// uselocale(LC_NUMERIC=C); HMAC byte-equivalence preserved across LC_NUMERIC
// drift (Layer 2 of wire-format-byte-preservation-discipline.md).
//
// At .A the emit format is placeholder "<name>=stub\n"; .B replaces with
// tt::cfg_emit_synthetic_field<T>(d, idx, buf+pos, cap-pos) real per-type emit.
//
// v5.15.5.F.4d.1.A — NEW (Path γ first canonical consumer).

#pragma once

#include <cstdio>
#include <cstring>
#include <locale.h>           // uselocale + newlocale per Layer 2 discipline
#include "../FixedPoint/FixedPointN.hpp"     // FPN_Binary<F> — required before CfgFieldRegistry.hpp
#include "CfgFieldRegistry.hpp"

// Canonical body emit — locale-pinned per Layer 2.
// Walks per-core descriptors first, then global (canonical wire order).
// Returns body length (bytes written, excluding NUL terminator).
//
// At .A: emit produces empty body since zero rows flagged STAMP_BOUND_CFG_DERIVED.
// .B activates real per-type emit via tt::cfg_emit_synthetic_field<T>.
inline size_t STAMP_BOUND_CFG_emit_canonical_body(char* buf, size_t cap) {
    size_t pos = 0;

    // Layer 2 locale pin: thread-local uselocale per
    // wire-format-byte-preservation-discipline.md § Layer 2.
    locale_t pinned = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t prev = (locale_t)0;
    if (pinned) prev = uselocale(pinned);

    // Per-core walk first (canonical body order — per-core before global).
    CFG_FIELD_FOR_EACH_SET_BIT(g_per_node_cfg_stamp_bound_cfg_derived_mask.words, idx, {
        const CfgFieldDescriptor& d = g_per_node_cfg_field_descriptors[idx];
        if (pos >= cap) break;
        // .A placeholder: "<name>=stub\n". .B replaces with
        // tt::cfg_emit_synthetic_field<T>(d, idx, buf+pos, cap-pos).
        int n = snprintf(buf + pos, cap - pos, "%s=stub\n", d.cfg_field_name);
        if (n > 0 && static_cast<size_t>(n) < cap - pos) {
            pos += static_cast<size_t>(n);
        }
    });

    // Global walk.
    CFG_FIELD_FOR_EACH_SET_BIT(g_global_cfg_stamp_bound_cfg_derived_mask.words, idx, {
        const CfgFieldDescriptor& d = g_global_cfg_field_descriptors[idx];
        if (pos >= cap) break;
        int n = snprintf(buf + pos, cap - pos, "%s=stub\n", d.cfg_field_name);
        if (n > 0 && static_cast<size_t>(n) < cap - pos) {
            pos += static_cast<size_t>(n);
        }
    });

    if (pinned) {
        uselocale(prev);
        freelocale(pinned);
    }
    return pos;
}
