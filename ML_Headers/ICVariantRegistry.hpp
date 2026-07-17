// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[ML_Headers/ICVariantRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[IC computation variant registry (v5.14.1.F) — Spearman today (RollingIC's real behavior despite the generic name); future variants = 1 row + dispatch auto-flows]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_IC_VARIANT]
//======================================================================================================
// X-macro registry for IC (Information Coefficient) computation variants
// on ConfidenceScorer. Operator selects active variant via cfg.confidence_ic_variant.
//
// **Today's truth (verified 2026-05-09):** the existing `RollingIC` struct
// at `ML_Headers/ConfidenceScore.hpp:57` ALREADY computes Spearman rank
// correlation (ranks both arrays then Pearson on ranks = Spearman). Despite
// the generic struct name, behavior has been Spearman since v5.x.x. The
// registry exposes this AS Spearman (variant 0, default).
//
// Future variants (Pearson on raw values, Kendall's tau, partial correlation,
// distance correlation, etc.) slot in as ONE-LINE additions to the
// FOREACH_IC_VARIANT(X) macro below. Adding a variant:
//   1. Implement the new variant's Init/Push/Compute fns
//   2. Add an X(name, ...) entry to the registry
//   3. Cfg dispatcher auto-routes when operator sets cfg.confidence_ic_variant
//
// **Why not refactor RollingIC → RollingICSpearman now?** Boundary-stable
// refactor rule (CLAUDE.local.md). Rename would cascade through 7 production
// + ~10 test sites for no functional gain. Doc-fix the misleading name +
// add the registry; future variants build alongside without disturbing
// existing wiring.
//
// X-macro registry-pattern alignment: this is a multi-site additions category
// (cfg field + struct field + dispatcher branch + test per variant).
// Registry collapses that to 1 line per variant.
//======================================================================================================
#ifndef IC_VARIANT_REGISTRY_HPP
#define IC_VARIANT_REGISTRY_HPP

//======================================================================
// [REGISTRY]_[FOREACH_IC_VARIANT]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [ML_INFERENCE] [FRAMEWORK_DISCIPLINE]]
// [REFERENCE]_[INVARIANT]_[H21]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[variant roster + count helper + the switch dispatcher; variant_id append-only (operator cfg files carry the numerics)]
// [COLUMN]_[variant_id]_[cfg.confidence_ic_variant value — STABLE, append-only]
// [COLUMN]_[name_tag/label_str]_[C identifier + display name]
// [COLUMN]_[compute_call]_[expression returning double IC; `cs` in scope at the dispatch site]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_IC_VARIANT(X)                                                              \
    /* v5.14.1.F — Spearman (default; existing RollingIC implementation). */               \
    X(0, spearman, "spearman",  RollingIC_Compute(&cs->ic))                                \
    /* Future variants — slot here as 1-line additions: */                                 \
    /* X(1, pearson,  "pearson",  RollingICPearson_Compute(&cs->ic_pearson)) */            \
    /* X(2, kendall,  "kendall",  RollingICKendall_Compute(&cs->ic_kendall)) */

//------------------------------------------------------------------------------
// VARIANT-COUNT HELPER
//------------------------------------------------------------------------------
// Compile-time count of registered IC variants. Used by tests to assert
// "all expected variants present" — catches accidental row deletion.
//======================================================================================================

#define IC_VARIANT_COUNT_ONE(id, name, label, compute) +1
#define FOREACH_IC_VARIANT_COUNT  (0 FOREACH_IC_VARIANT(IC_VARIANT_COUNT_ONE))

//------------------------------------------------------------------------------
// DISPATCHER MACRO
//------------------------------------------------------------------------------
// Computes IC for the variant selected by `variant` arg. Used by new
// ConfidenceScorer_ComputeICVariant + drift detection / display sites
// that want to honor cfg.confidence_ic_variant.
//
// Caller scope must have `cs` (ConfidenceScorer*) available; the
// compute_call expressions reference it.
//
// Cold-start (variant out of range): falls through to the default case
// at the end of the switch — returns 0.0 (no IC info; safe).
//======================================================================================================

#define IC_VARIANT_DISPATCH_CASE(id, name, label, compute) \
    case (id): return (compute);

// Caller usage:
//   double ic = IC_VARIANT_COMPUTE(scorer_ptr, scorer_ptr->active_ic_variant);
//
// Inside ConfidenceScorer_ComputeICVariant:
//   double ic_now = IC_VARIANT_COMPUTE(cs, cs->active_ic_variant);
#define IC_VARIANT_COMPUTE(cs, variant)                                                    \
    ([&]() -> double {                                                                     \
        switch ((variant)) {                                                               \
            FOREACH_IC_VARIANT(IC_VARIANT_DISPATCH_CASE)                                   \
            default: return 0.0;  /* unknown variant → safe */                             \
        }                                                                                  \
    }())
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// X(variant_id, name_tag, label_str, compute_call)
//
//   variant_id   — integer enum value used by cfg.confidence_ic_variant.
//                  Stable; do NOT renumber existing entries (operators have
//                  cfg files referencing these). Append-only.
//   name_tag     — short C identifier used for struct field naming if
//                  per-variant fields are ever added (currently only
//                  spearman has a backing field on ConfidenceScorer.ic).
//   label_str    — human-readable name for log/TUI display.
//   compute_call — call-site expression that returns a double IC ∈ [-1, 1].
//                  Receives `cs` (ConfidenceScorer*) in scope at the dispatch
//                  site. e.g. `RollingIC_Compute(&cs->ic)` for spearman.
//
// IMPORTANT: compute_call is evaluated at the dispatcher; the macro doesn't
// know which struct field holds the variant's state. Keeps the registry
// independent of ConfidenceScorer's internal layout.
//======================================================================
// [END_REGISTRY]_[FOREACH_IC_VARIANT]
//======================================================================

#endif // IC_VARIANT_REGISTRY_HPP
