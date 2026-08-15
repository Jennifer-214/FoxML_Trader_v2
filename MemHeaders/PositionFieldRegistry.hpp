// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/PositionFieldRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [FRAMEWORK_DISCIPLINE] [PERSISTENCE] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the Position<F> field SSoT — declaration order IS the wire order of the SHARDED snapshot's Position blob; append-only under SHARDED_SNAPSHOT_VERSION]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_POSITION_FIELD]   (+ the SKIP_PERSIST placeholder macro + PERSIST_KIND dispatch share the block)
// [REFERENCE]_[INVARIANT]_[[H9] [H21]]
// [REFERENCE]_[DESIGN_SPEC]_[persisted-struct-with-ephemeral-field-coexistence-pattern]
//======================================================================================================

#pragma once

//======================================================================
// [REGISTRY]_[FOREACH_POSITION_FIELD]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [FRAMEWORK_DISCIPLINE] [PERSISTENCE] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[9 PERSIST rows in wire order (DO NOT REORDER — SHARDED_SNAPSHOT_VERSION locked) + the empty SKIP_PERSIST future-extension macro + the PERSIST_KIND token-paste dispatch]
// [COLUMN]_[name]_[Position<F> member identifier]
// [COLUMN]_[type]_[C storage type (Money / uint64_t / int8_t)]
// [COLUMN]_[init]_[Position_Init value]
// [COLUMN]_[persist_kind]_[PERSIST = intended for the wire; SKIP_PERSIST = struct-only, cleared on Init. NOTE (E.1.2/D-289): the live sharded wire dumps Position WHOLE and does NOT read this column — see the dispatch section note]
// [COLUMN]_[doc]_[cache-tier + semantics note]
// [REFERENCE]_[DESIGN_SPEC]_[[wire-format-byte-preservation-discipline] [hot-side-array-element-alignment-for-sparse-access.md] [persisted-struct-with-ephemeral-field-coexistence-pattern.md] [pre-post-cfg-registry-split-for-emit-order-preservation.md] [slot-state-foreach-registry-with-storage-routing.md]]
// [REFERENCE]_[INVARIANT]_[H21]
//======================================================================
// [CODE]
//======================================================================
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

//------------------------------------------------------------------
// [SECTION]_[FOREACH_POSITION_FIELD_SKIP_PERSIST — empty placeholder]
//------------------------------------------------------------------
// v5.15.5.C.4 POS.2 added 2 SKIP_PERSIST fields (exit_fill_price, is_maker)
// here. v5.15.5.C.5 REVERTED those fields to OMS sibling arrays per the
// `slot-state-foreach-registry-with-storage-routing.md` decision tree:
// sparse-access per-slot ephemeral state goes to OMS sibling SoA arrays;
// Position holds only PERSIST state. The revert allows Position to be
// PERSIST-only + alignas(64); at Ship-A's 16B Money that is 128B = 2 cache
// lines exact, eliminating hot-path cache-line straddle (per
// `hot-side-array-element-alignment-for-sparse-access.md` first canonical
// application). (It was 184B/192B/3 lines in the 24B-Money era.)
//
// This empty macro is RETAINED as future-extension infrastructure. If a
// future field genuinely benefits from Position-locality (e.g., a transient
// flag co-accessed with PERSIST fields in slow-path), it can be added here.
//
// ⚠️ E.1.2/D-289 — READ BEFORE ADDING A SKIP_PERSIST FIELD: the PERSIST_KIND
// filter's only consumers were Portfolio_Save/Load, which are DELETED. The
// live sharded serializer dumps `sizeof(Position<F>)` WHOLE and never
// consults this column, so a SKIP_PERSIST field added today would silently
// reach the wire anyway. The `sizeof(Position) - POSITION_PERSIST_BYTES == 0`
// static_assert in Portfolio.hpp is the tripwire that red-builds on that,
// forcing an explicit wire decision (a SHARDED_SNAPSHOT_VERSION bump, H21).
// Adding a SKIP_PERSIST field means EITHER re-teaching the sharded serializer
// to filter, OR accepting it on the wire and bumping the version.
//
// Pattern remains composable with `pre-post-cfg-registry-split-for-emit-order-preservation.md`
// (the manual `_pad_pos` plays the role of a "sister registry boundary"
// between PRE (PERSIST) and any future POST (SKIP_PERSIST) entries).
#define FOREACH_POSITION_FIELD_SKIP_PERSIST(X) /* empty; v5.15.5.C.5 revert; see comment above */

//------------------------------------------------------------------
// [SECTION]_[PERSIST_KIND token-paste dispatch]
//------------------------------------------------------------------
// Originally used by Portfolio_Save / Portfolio_Load (POS.2) to filter PERSIST
// fields from SKIP_PERSIST. Those two serializers were DELETED at E.1.2/D-289,
// so this dispatch has NO live consumer today — it is retained as the ready-made
// filter for whenever a SKIP_PERSIST field is actually introduced (see the
// warning in the SKIP_PERSIST section above). All 9 rows are PERSIST, so the
// filter is a no-op on the current registry either way.
//
// Pattern mirrors FOREACH_OMS_FIELD's STORAGE_KIND dispatch shipped in
// v5.15.5.C.3 Phase 3b — same X-macro discipline.
#define PERSIST_KIND_EMIT_PERSIST(op, ...)      op(__VA_ARGS__)
#define PERSIST_KIND_EMIT_SKIP_PERSIST(op, ...) /* skip — not in wire format */
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// FOREACH_POSITION_FIELD(X) — tuple shape:
//   X(name, type, init, persist_kind, doc)
//
// FIELD ORDER MATTERS: the SHARDED_SNAPSHOT_VERSION wire format is locked to
// the current declaration order — the sharded serializer blob-dumps Position,
// so declaration order IS byte order. New PERSIST fields are APPENDED only (per
// `DESIGN_SPECS/wire-format-byte-preservation-discipline.md`), and any append
// changes sizeof → trips the layout locks → forces the H21 version bump.
//
// Order also matches the original Portfolio.hpp manual struct's cache-layout
// intent (hot-path fields first → warm fields → cold fields).
//
// X-macro registry for Position<F> struct generation per
// `DESIGN_SPECS/persisted-struct-with-ephemeral-field-coexistence-pattern.md`.
// PERSIST_KIND column DECLARES intended wire-format participation:
//
//   PERSIST       — field is intended for the wire (today: reaches disk via the
//                   sharded snapshot's whole-Position blob dump, under
//                   SHARDED_SNAPSHOT_VERSION)
//   SKIP_PERSIST  — field lives in Position struct but is NOT intended for the wire
//                   (cleared on Init; never restored from snapshot). NONE today —
//                   and see the D-289 warning above before adding one.
//
// POS.1: all 9 existing fields registered as PERSIST in their
// CURRENT declaration order — preserves cache-layout discipline (hot fields
// first, warm middle, cold last) AND wire-format byte layout. Static_asserts on
// sizeof(Position) + offsetof(...) lock the layout.
//
// POS.2: added the SKIP_PERSIST machinery for Phase G's exit-side derive
// cascade; the 2 fields later REVERTED to OMS sibling arrays at v5.15.5.C.5
// (see the SKIP_PERSIST section note), leaving the registry all-PERSIST and the
// wire byte-identical.
//
// E.1.2/D-289: the standalone PORTFOLIO snapshot format (and its Save/Load
// serializers, which this column originally fed) is RETIRED. Position's bytes now
// ride ONLY the sharded wire. The column and its dispatch survive as declaration +
// ready-made filter, not as a live consumer.
//
// FUTURE FIELD ADDITIONS:
// 1 row in FOREACH_POSITION_FIELD with explicit PERSIST_KIND. Future field
// = 1-line addition; choice IS the design decision (encoded once, enforced
// at compile time). Closes the "Position struct extensions require snapshot
// version bump" class per the structural-fix-preferred gradient.
//======================================================================
// [END_REGISTRY]_[FOREACH_POSITION_FIELD]
//======================================================================
