// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [POSITION FIELD REGISTRY]  (v5.15.5.C.4 Phase POS — FOREACH_POSITION_FIELD with PERSIST_KIND column)
//======================================================================================================
//
// X-macro registry for Position<F> struct generation per
// `DESIGN_SPECS/persisted-struct-with-ephemeral-field-coexistence-pattern.md`.
// PERSIST_KIND column drives wire-format participation:
//
//   PERSIST       — field is part of Portfolio_Save / Portfolio_Load wire format
//                   (PORTFOLIO_SNAPSHOT_VERSION=5 byte layout)
//   SKIP_PERSIST  — field lives in Position struct but NOT in wire format
//                   (cleared on Init; never persisted; never restored from snapshot)
//
// POS.1 (this commit): all 9 existing fields registered as PERSIST in their
// CURRENT declaration order — preserves cache-layout discipline (hot fields
// first, warm middle, cold last) AND wire-format byte layout
// (PORTFOLIO_SNAPSHOT_VERSION=5 byte-identical). Static_asserts on
// sizeof(Position) + offsetof(...) lock the layout.
//
// POS.2 (subsequent commit): adds 2 SKIP_PERSIST fields (`exit_fill_price` +
// `is_maker`) for Phase G's exit-side derive cascade. Wire format STILL
// byte-identical via Save/Load filter walking only PERSIST fields. No
// PORTFOLIO_SNAPSHOT_VERSION bump.
//
// FUTURE FIELD ADDITIONS:
// 1 row in FOREACH_POSITION_FIELD with explicit PERSIST_KIND. Future field
// = 1-line addition; choice IS the design decision (encoded once, enforced
// at compile time). Closes the "Position struct extensions require snapshot
// version bump" class per CLAUDE.md item 19.
//======================================================================================================

#pragma once

//======================================================================================================
// FOREACH_POSITION_FIELD(X) — tuple shape:
//   X(name, type, init, persist_kind, doc)
//
// FIELD ORDER MATTERS: PORTFOLIO_SNAPSHOT_VERSION=5 wire format is locked to
// the current declaration order. New PERSIST fields are APPENDED only (per
// `DESIGN_SPECS/wire-format-byte-preservation-discipline.md`).
//
// Order also matches the original Portfolio.hpp manual struct's cache-layout
// intent (hot-path fields first → warm fields → cold fields).
//======================================================================================================
#define FOREACH_POSITION_FIELD(X)                                                                                       \
    /* PERSIST fields (current wire format) — DO NOT REORDER */                                                          \
    X(take_profit_price,   Money,           Money_Zero() ,    PERSIST,      "hot: TP; modified by trailing TP on slow path")    \
    X(stop_loss_price,     Money,           Money_Zero() ,    PERSIST,      "hot: SL; modified by trailing SL on slow path")    \
    X(quantity,            Money,           Money_Zero() ,    PERSIST,      "warm: +long, -short")                              \
    X(entry_price,         Money,           Money_Zero() ,    PERSIST,      "warm: fill price at open")                         \
    X(entry_fee,           Money,           Money_Zero() ,    PERSIST,      "warm: fee paid at fill")                           \
    X(original_tp,         Money,           Money_Zero() ,    PERSIST,      "cold: TP at fill; never modified")                 \
    X(original_sl,         Money,           Money_Zero() ,    PERSIST,      "cold: SL at fill; never modified")                 \
    X(entry_timestamp_us,  uint64_t, 0,                PERSIST,      "v5.11.65 wall-clock entry time (microseconds)")    \
    X(pair_index,          int8_t,   -1,               PERSIST,      "partial-exit pairing; -1=unpaired, 0-15=pair idx")

//======================================================================================================
// FOREACH_POSITION_FIELD_SKIP_PERSIST(X) — empty placeholder.
//
// v5.15.5.C.4 POS.2 added 2 SKIP_PERSIST fields (exit_fill_price, is_maker)
// here. v5.15.5.C.5 REVERTED those fields to OMS sibling arrays per the
// `slot-state-foreach-registry-with-storage-routing.md` decision tree:
// sparse-access per-slot ephemeral state goes to OMS sibling SoA arrays;
// Position holds only PERSIST state. The revert allows Position to be 184B
// (PERSIST-only) + alignas(64) → 192B = 3 cache lines exact, eliminating
// hot-path cache-line straddle (per
// `hot-side-array-element-alignment-for-sparse-access.md` first canonical
// application).
//
// This empty macro is RETAINED as future-extension infrastructure. If a
// future field genuinely benefits from Position-locality (e.g., a transient
// flag co-accessed with PERSIST fields in slow-path), it can be added here.
// The PERSIST_KIND filter machinery (Portfolio_Save/Load) remains intact.
//
// Pattern remains composable with `pre-post-cfg-registry-split-for-emit-order-preservation.md`
// (the manual `_pad_pos` plays the role of a "sister registry boundary"
// between PRE (PERSIST) and any future POST (SKIP_PERSIST) entries).
//======================================================================================================
#define FOREACH_POSITION_FIELD_SKIP_PERSIST(X) /* empty; v5.15.5.C.5 revert; see comment above */

//======================================================================================================
// PERSIST_KIND token-paste dispatch.
//
// Used by Portfolio_Save / Portfolio_Load (POS.2) to filter PERSIST fields
// from SKIP_PERSIST. POS.1 doesn't need filtering (all fields PERSIST), but
// dispatch is defined here for POS.2 + future use.
//
// Pattern mirrors FOREACH_OMS_FIELD's STORAGE_KIND dispatch shipped in
// v5.15.5.C.3 Phase 3b — same X-macro discipline.
//======================================================================================================
#define PERSIST_KIND_EMIT_PERSIST(op, ...)      op(__VA_ARGS__)
#define PERSIST_KIND_EMIT_SKIP_PERSIST(op, ...) /* skip — not in wire format */
