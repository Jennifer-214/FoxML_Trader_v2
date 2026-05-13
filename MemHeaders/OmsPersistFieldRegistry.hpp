// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [OMS PERSIST FIELD REGISTRY — v5.15.5.C.2 (S3a-W)]
//======================================================================================================
// X-macro registry covering all OMS fields persisted in the sharded
// snapshot. Closes a Class 18 mirror at function-composition level —
// pre-S3a-W each persisted field required THREE parallel sites:
//   1. save-time fwrite at ShardedSnapshot_Save
//   2. load-time fread into a tmp_<name> at ShardedSnapshot_Load
//   3. load-time commit assignment to state->oms-><name> at the load tail
// Adding a new persisted field required touching all 3 sites in lockstep.
// Drift between sites silently corrupts wire format.
//
// Post-S3a-W: adding a new persisted OMS field = ONE row in the registry
// below. Save + load read + load commit auto-generate via FOREACH
// expansion (CLAUDE.md item 13 X-macro registry + item 19 structural
// fix preferred).
//
// Wire format is byte-preserved: FOREACH expands in the same canonical
// order as the legacy hand-written block (per CLAUDE.md item 15
// Parity-tested-by-construction + DESIGN_SPECS/wire-format-byte-
// preservation-discipline.md). No snapshot version bump required.
//
// Tuple shape: X(name, type, kind, mask)
//   name — field name. Wire stream uses this as the per-row position
//          (NOT the byte name); load tmp_<name> + commit to
//          state->oms-><name> (DIRECT kind) or extract/set via
//          oms_state_flags bit (BIT kind).
//   type — wire type. sizeof(type) bytes consumed per row.
//   kind — DIRECT (read/write state->oms->name directly) or BIT
//          (save: extract from oms_state_flags; commit: set/clear bit).
//   mask — UNQUALIFIED mask name (e.g. MASK_OMS_STATE_KILL_SWITCH_TRIPPED)
//          for BIT kind; ignored for DIRECT kind (pass 0).
//
// kill_switch_tripped uses BIT kind post-S3a because S3a folded the
// uint8_t field into oms_state_flags. Wire format is still int (4
// bytes); save extracts the bit, commit sets/clears the bit.
//
// Cross-references:
//   CLAUDE.md item 13 (X-macro registry pattern)
//   CLAUDE.md item 15 (Parity-tested-by-construction — wire format)
//   CLAUDE.md item 19 (structural-fix-preferred for recurring class)
//   CLAUDE.md item 21 (AUTOPOPULATE companion — same shape applied to
//                       multi-site struct assembly)
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md
//   DESIGN_SPECS/wire-format-byte-preservation-discipline.md
//   v5.15.5.B.7 FOREACH_CORE_CTX_INIT_FIELD + CORE_CTX_INIT_AUTOPOPULATE —
//     sister registry (per-core init/reset Class-18 closure)
//
// USAGE (call site must have these in scope):
//   - `state` — EventLoopState<F>* (parameter or local)
//   - `f`     — FILE* (opened wb / rb at call site)
//   - For save: label `fail:` for `goto fail` on I/O failure
//   - For load: this returns 0 from the enclosing fn on I/O failure
//   - `oms_state_flags` field on state->oms with BITMAP_* primitives
//
// CALL FORMS:
//   FOREACH_OMS_PERSIST_FIELD(OMS_PERSIST_DO_SAVE)     — save site
//   FOREACH_OMS_PERSIST_FIELD(OMS_PERSIST_DECLARE_TMP) — load tmp decl
//   FOREACH_OMS_PERSIST_FIELD(OMS_PERSIST_DO_READ)    — load fread
//   FOREACH_OMS_PERSIST_FIELD(OMS_PERSIST_DO_COMMIT)  — load commit
//======================================================================================================
#ifndef OMS_PERSIST_FIELD_REGISTRY_HPP
#define OMS_PERSIST_FIELD_REGISTRY_HPP

#include <stdint.h>
#include "BitmapMacros.hpp"
#include "OmsStateFlagRegistry.hpp"  // MASK_OMS_STATE_* constants

//======================================================================================================
// [REGISTRY DEFINITION — canonical wire order]
//======================================================================================================
// Order MUST match the legacy hand-written save/load (CLAUDE.md item 15
// wire-format byte-preservation). Adding new fields appends at the END
// (preserves byte offsets of existing entries).
//
// Wire layout (snapshot v7; v5.15.5.C.2.1 fixup corrected MEDIUM-1 comment
// drift — pre-S3a-W block produced same wire as v7 since the v6→v7 bump
// added per-core fields, not OMS-block fields):
//   balance              FPN<F>     [sizeof(FPN<F>) bytes]
//   realized_pnl         FPN<F>     [sizeof(FPN<F>) bytes]
//   ks_peak_balance      FPN<F>     [sizeof(FPN<F>) bytes]
//   kill_switch_tripped  int        [4 bytes]  ← post-S3a: BIT extraction
//   total_fees           FPN<F>     [sizeof(FPN<F>) bytes]
//   total_maker_fees     FPN<F>     [sizeof(FPN<F>) bytes]
//   total_taker_fees     FPN<F>     [sizeof(FPN<F>) bytes]
//   maker_fills_count    uint32_t   [4 bytes]
//   taker_fills_count    uint32_t   [4 bytes]
//======================================================================================================
#define FOREACH_OMS_PERSIST_FIELD(X)                                                                  \
    X(balance,             FPN<F>,    DIRECT, 0)                                                       \
    X(realized_pnl,        FPN<F>,    DIRECT, 0)                                                       \
    X(ks_peak_balance,     FPN<F>,    DIRECT, 0)                                                       \
    X(kill_switch_tripped, int,       BIT,    MASK_OMS_STATE_KILL_SWITCH_TRIPPED)                      \
    X(total_fees,          FPN<F>,    DIRECT, 0)                                                       \
    X(total_maker_fees,    FPN<F>,    DIRECT, 0)                                                       \
    X(total_taker_fees,    FPN<F>,    DIRECT, 0)                                                       \
    X(maker_fills_count,   uint32_t,  DIRECT, 0)                                                       \
    X(taker_fills_count,   uint32_t,  DIRECT, 0)

//======================================================================================================
// [PER-KIND DISPATCH HELPERS]
//======================================================================================================
// Save: produce the value to write per field.
//   DIRECT(name)      → state->oms->name
//   BIT(name, mask)   → 1 or 0 based on the named bit in oms_state_flags
#define OMS_PERSIST_SAVE_VAL_DIRECT(name, mask)  (state->oms->name)
#define OMS_PERSIST_SAVE_VAL_BIT(name, mask)     \
    (BITMAP_IS_SET(state->oms->oms_state_flags, tt::mask) ? 1 : 0)

// Commit: assign tmp_<name> back to OMS. Both kinds expand to a `{ }` block
// (compound statement) matching the OMS_PERSIST_DO_SAVE shape — FOREACH
// expansion produces a sequence of self-terminating compound statements,
// no trailing `;` ambiguity (v5.15.5.C.2.1 LOW-1 close).
//   DIRECT(name, type, mask) → { state->oms->name = tmp_<name>; }
//   BIT(name, type, mask)    → { set or clear the named bit in oms_state_flags }
#define OMS_PERSIST_COMMIT_DIRECT(name, type, mask) \
    { state->oms->name = tmp_##name; }
#define OMS_PERSIST_COMMIT_BIT(name, type, mask)                                            \
    {                                                                                        \
        if (tmp_##name) state->oms->oms_state_flags |= (uint8_t)(tt::mask);                  \
        else            state->oms->oms_state_flags &= (uint8_t)(~(uint8_t)(tt::mask));      \
    }

//======================================================================================================
// [OMS_PERSIST_DO_* — call-site expansion macros]
//======================================================================================================
// Save: write the field via fwrite. Assumes call site has FILE* f + a
// `fail:` label reachable via `goto fail` on I/O failure.
#define OMS_PERSIST_DO_SAVE(name, type, kind, mask)                                                 \
    {                                                                                                \
        type _v_save_##name = OMS_PERSIST_SAVE_VAL_##kind(name, mask);                               \
        if (fwrite(&_v_save_##name, sizeof(type), 1, f) != 1) goto fail;                             \
    }

// Load: declare zero-initialized tmp_<name> for each field. C++ value-init
// guarantees zero for primitives; FPN<F> has default member init for its
// padding (CLAUDE.md item 27), so `= {}` is safe across all wire types.
#define OMS_PERSIST_DECLARE_TMP(name, type, kind, mask) type tmp_##name = {};

// Load: fread into tmp_<name>. Caller scope must `return 0` from the
// enclosing function (the load function) on I/O failure.
#define OMS_PERSIST_DO_READ(name, type, kind, mask)                                                  \
    if (fread(&tmp_##name, sizeof(type), 1, f) != 1) { fclose(f); return 0; }

// Load: dispatch commit by kind.
#define OMS_PERSIST_DO_COMMIT(name, type, kind, mask) OMS_PERSIST_COMMIT_##kind(name, type, mask)

#endif  // OMS_PERSIST_FIELD_REGISTRY_HPP
