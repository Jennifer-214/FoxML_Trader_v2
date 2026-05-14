// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
//======================================================================================================
// [STRATEGY CATEGORIES]
//======================================================================================================
// v5.15.5.F.4b — categorical-applicability bitmap enum for strategy classification.
// Used by CfgFieldRegistry's applies_to_strategy_cat column to declare WHICH KIND
// of strategy a cfg field is relevant to (capability-based, not instance-named).
//
// Pattern: DESIGN_SPECS/categorical-tag-applicability-pattern.md
// CLAUDE.md item 19 (structural fix preferred), item 20 (BITMAP_*).
//
// VOCABULARY DISCIPLINE (5 rules per spec):
//   1. Functional capability, not implementation detail
//      ✓ STRAT_CAT_USES_BANDIT (what)  vs  ✗ STRAT_CAT_INSTANTIATES_BANDIT_T (how)
//   2. Operator-meaningful (these names appear in GUI tooltips + cfg.example)
//   3. Stable — renames cascade through every consumer; pick names that survive 3+ ships
//   4. Promoted from observed clustering (≥3 instances share the capability)
//   5. Tiered bit allocation: CORE / SPECIFIC / EXPERIMENTAL ranges below
//
// .F.4h: full audit pass + per-strategy category mask declaration in FOREACH_STRATEGY
// (tuple gains a category-mask column; this enum's bit allocation locks at .F.4b).
//======================================================================================================
#pragma once
#include <cstdint>

enum StrategyCategory : uint32_t {
    // ─── TIER CORE (bits 0-7) — stable across major versions ─────────────────
    STRAT_CAT_STATIC_RULES         = 1u << 0,    // SimpleDip, EmaCross — fixed-rule strategies
    STRAT_CAT_REGRESSION_DRIVEN    = 1u << 1,    // Momentum, MeanReversion — slope/R²-driven
    STRAT_CAT_ML                   = 1u << 2,    // ML inference-driven entry/exit
    STRAT_CAT_ONLINE_LEARNING      = 1u << 3,    // updates parameters during run (bandit, ridge)

    // ─── TIER SPECIFIC (bits 8-23) — stable across minor versions ────────────
    STRAT_CAT_USES_BANDIT          = 1u << 8,    // dispatches via Exp3-IX or Thompson bandit
    STRAT_CAT_USES_THOMPSON        = 1u << 9,    // specifically Thompson sampling (subtype)
    STRAT_CAT_USES_RIDGE           = 1u << 10,   // ridge-regression-blended ensemble
    STRAT_CAT_USES_CONFIDENCE      = 1u << 11,   // composite confidence scoring
    STRAT_CAT_LONG_ONLY            = 1u << 12,   // buy-only; cannot short
    STRAT_CAT_LONG_AND_SHORT       = 1u << 13,   // both directions
    STRAT_CAT_REGIME_AWARE         = 1u << 14,   // gates on regime classification
    STRAT_CAT_USES_DEPTH_DATA      = 1u << 15,   // requires order-book depth feed
    STRAT_CAT_USES_FLOW_DATA       = 1u << 16,   // requires trade-flow features

    // ─── TIER EXPERIMENTAL (bits 24-31) — may be removed; aliased on removal ─
    // (none initial; reserved for v5.16+ category proposals)

    // ─── SENTINEL ────────────────────────────────────────────────────────────
    STRAT_CAT_ALL                  = 0xFFFFFFFFu, // applies universally (default for cohort fields)
};

// Bitmap overflow guard per DESIGN_SPECS/bitmap-overflow-protection-discipline.md
// CLAUDE.local.md going-forward rule "Bitmap overflow static_assert is mandatory" (2026-05-14)
static_assert(STRAT_CAT_USES_FLOW_DATA < (1ull << 32),
              "StrategyCategory bitmap overflowed uint32_t — upgrade to uint64_t "
              "OR split orthogonal axes into separate enums");
