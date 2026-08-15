// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/NodeCtxPersistRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [FRAMEWORK_DISCIPLINE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ordered per-node persist WIRE SPEC — 29 rows in exact fwrite-call order (SCALAR/BIT/PAD/DELEGATE x COMMIT/NO_COMMIT); SAVE/READ/COMMIT projections auto-generate the serializer walk]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_NODE_PERSIST_FIELD]
//   - [MACRO]_[NPF_PROJECT_SAVE]   (+ READ + COMMIT projections)
// [REFERENCE]_[DECISION]_[[D-291] [D-302] [D-305]]
// [REFERENCE]_[INVARIANT]_[[H9] [H15] [H21]]
// [REFERENCE]_[CLASS]_[18]
// [REFERENCE]_[DESIGN_SPEC]_[[registry-tuple-as-single-source-of-truth] [wire-format-byte-preservation-discipline] [autopopulate-pattern-for-production-caller-class]]
//======================================================================================================
// E.1.2 D-305 tail (the hybrid flat-registry + delegates shape locked by D-291/BLK-1,
// forcing-function architecture locked by D-302): ONE ordered registry replaces the
// hand-written per-node fwrite/fread/commit loops in ShardedSnapshotPersist.hpp.
// Dropping a wire op is now dropping a ROW — caught by the count-lock + the frozen
// v10 byte-golden (tests/sharded_snapshot_v10_golden.hpp), closing the
// node_gross_wins→$0.00 silent-field-drop class (TECH_DEBT-196 mechanism).
//
// ROW ORDER IS THE WIRE ORDER. Emission order == row order == the retired hand-loop
// order, BY CONSTRUCTION. The three confidence doubles deliberately sit BETWEEN the
// pnl_feeder and confidence delegates (the D-110 interleave: struct-declaration order
// is NOT wire order — a registry emitting struct order would silently hoist 24 bytes).
// Reordering rows = changing the wire = SHARDED_SNAPSHOT_VERSION bump (H21).
//
// OMS sibling precedent: MemHeaders/OmsFieldRegistry.hpp PERSIST view (:475-598) —
// same token-paste projection discipline, same static_assert(false) on undesigned
// combos. NodeCtx needs what OMS doesn't: ordered PAD rows, DELEGATE rows (sub-walkers
// invoked AT their wire position), and a NO_COMMIT kind (rows that READ off the wire
// for offset-correctness but are deliberately not applied at commit).
//======================================================================================================
#ifndef NODE_CTX_PERSIST_REGISTRY_HPP
#define NODE_CTX_PERSIST_REGISTRY_HPP

//======================================================================
// [REGISTRY]_[FOREACH_NODE_PERSIST_FIELD]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[29 wire rows, 1944B per node block at snapshot v10 — 20 SCALAR + 1 BIT + 2 PAD + 3 DELEGATE + the 3 interleave doubles; tuple X(NAME, TYPE, STORAGE_KIND, STORAGE_MASK, COMMIT_KIND)]
// [COLUMN]_[NAME]_[NodeContext field name == NodeSnap staging field name (unified; PAD rows use scratch, no staging field)]
// [COLUMN]_[type]_[C++ element type — sizeof() drives the fwrite/fread width]
// [COLUMN]_[STORAGE_KIND]_[SCALAR = plain field · BIT = node_state_flags bit as a 1-byte 0/1 wire byte · PAD = zero-written alignment bytes · DELEGATE = field-by-field sub-registry walker at this wire position]
// [COLUMN]_[STORAGE_MASK]_[BIT: the NODE_STATE_FLAG_* token · PAD: byte width · DELEGATE: the walker fn-family prefix (<PREFIX>_FieldwiseWrite/Read + _CommitPersistedFields) · SCALAR: 0]
// [COLUMN]_[COMMIT_KIND]_[COMMIT = applied to NodeContext at load-commit · NO_COMMIT = read off the wire (offset correctness) but deliberately NOT applied]
// [REFERENCE]_[DECISION]_[D-305]
// [REFERENCE]_[INVARIANT]_[H9]
//======================================================================
// [CODE]
//======================================================================
// NO_COMMIT rows (the v5.11.15 leak class — see ShardedSnapshotPersist.hpp commit
// site for the full history): strategy_id comes from cfg at boot, never the snapshot;
// strategy_state_kind restored from a snapshot desyncs kind-vs-allocated-state and
// leaked the strategy_state pointer. Both STAY on the wire (v10 byte layout) — they
// READ (consuming their bytes keeps every later offset correct) and no-op at commit.
//
// v5.4.3 (snapshot v5) history rides node_gross_wins/losses + idle_cycles (Class 4:
// added v4.7.25, silently never persisted — Stats avg_win/avg_loss/profit_factor read
// $0.00 after restart). THE founding instance of the silent-field-drop class this
// registry structurally closes.
#define FOREACH_NODE_PERSIST_FIELD(X)                                                                \
    /* identity / sizing (wire rows 1-5) */                                                          \
    X(strategy_id,            uint8_t,              SCALAR,   0,                  NO_COMMIT)          \
    X(resolved_strategy_id,   uint8_t,              SCALAR,   0,                  COMMIT)             \
    X(strategy_state_kind,    uint8_t,              SCALAR,   0,                  NO_COMMIT)          \
    X(_pad_ids,               uint8_t,              PAD,      1,                  NO_COMMIT)          \
    X(allocated_balance,      Money,                SCALAR,   0,                  COMMIT)             \
    /* counters (rows 6-15) */                                                                       \
    X(entries_processed,      uint64_t,             SCALAR,   0,                  COMMIT)             \
    X(exits_processed,        uint64_t,             SCALAR,   0,                  COMMIT)             \
    X(node_realized,          Money,                SCALAR,   0,                  COMMIT)             \
    X(node_fees,              Money,                SCALAR,   0,                  COMMIT)             \
    X(node_open_notional,     Money,                SCALAR,   0,                  COMMIT)             \
    X(node_wins,              uint32_t,             SCALAR,   0,                  COMMIT)             \
    X(node_losses,            uint32_t,             SCALAR,   0,                  COMMIT)             \
    X(node_gross_wins,        Money,                SCALAR,   0,                  COMMIT)             \
    X(node_gross_losses,      Money,                SCALAR,   0,                  COMMIT)             \
    X(idle_cycles,            uint32_t,             SCALAR,   0,                  COMMIT)             \
    /* spacing state (rows 16-18) */                                                                 \
    X(last_entry_price,       Money,                SCALAR,   0,                  COMMIT)             \
    X(last_entry_tick,        uint64_t,             SCALAR,   0,                  COMMIT)             \
    X(sl_cooldown_remaining,  uint32_t,             SCALAR,   0,                  COMMIT)             \
    /* kill switch (rows 19-23) — kill bit wire-encoded as a 1-byte 0/1 (pre-.B.3 format preserved) */ \
    X(node_peak_balance,      Money,                SCALAR,   0,                  COMMIT)             \
    X(node_dd_pct,            Money,                SCALAR,   0,                  COMMIT)             \
    X(node_kill_tripped,      uint8_t,              BIT,      KILL_TRIPPED,       COMMIT)             \
    X(_pad_kill,              uint8_t,              PAD,      3,                  NO_COMMIT)          \
    X(node_ks_trips_total,    uint32_t,             SCALAR,   0,                  COMMIT)             \
    /* sub-struct delegates at their wire positions (rows 24-25; E.1.2 Step-2c, D-304) */            \
    X(regime_state,           RegimeState<F>,       DELEGATE, RegimeState,        COMMIT)             \
    X(pnl_feeder,             RegressionFeederX<F>, DELEGATE, RegressionFeederX,  COMMIT)             \
    /* the D-110 interleave: 3 confidence doubles BETWEEN feeder and confidence (rows 26-28) */      \
    X(staged_prediction,      double,               SCALAR,   0,                  COMMIT)             \
    X(active_prediction,      double,               SCALAR,   0,                  COMMIT)             \
    X(last_confidence,        double,               SCALAR,   0,                  COMMIT)             \
    /* confidence delegate (row 29; FOREACH_CONFIDENCE_PERSIST_FIELD, 1552B) */                      \
    X(confidence,             ConfidenceScorer,     DELEGATE, ConfidenceScorer,   COMMIT)

//------------------------------------------------------------------
// [SECTION]_[COMPILE-TIME COUNT SENTINEL — the parent count-lock]
//------------------------------------------------------------------
// Wire pin: EXACTLY 29 rows at snapshot v10. Adding/removing/reordering a row
// CHANGES the per-node wire → SHARDED_SNAPSHOT_VERSION bump + golden regen
// (regen/RENAME tests/sharded_snapshot_v10_golden.hpp) ride the SAME commit (H21).
// Sub-registry tripwires (regime ==7 · feeder ==3 · confidence ==7) live beside
// their registries — a delegate-internal drop is caught THERE; this lock catches
// parent-level row motion.
#define _NPF_COUNT_ONE(NAME, TYPE, SKIND, SMASK, CKIND) +1
constexpr int FOREACH_NODE_PERSIST_FIELD_COUNT =
    0 FOREACH_NODE_PERSIST_FIELD(_NPF_COUNT_ONE);
#undef _NPF_COUNT_ONE
// [ASSERT]_[REGISTRY_COVERAGE]_[FOREACH_NODE_PERSIST_FIELD_COUNT == 29 — the per-node wire row pin]
static_assert(FOREACH_NODE_PERSIST_FIELD_COUNT == 29,
              "Snapshot v10 per-node wire = EXACTLY 29 ordered rows (1944B/node). "
              "Adding/removing/reordering a row changes the wire format: bump "
              "SHARDED_SNAPSHOT_VERSION + regen/RENAME the version-named golden "
              "(tests/sharded_snapshot_v10_golden.hpp) in the SAME commit. See "
              "DESIGN_SPECS/wire-format-patterns/wire-format-byte-preservation-discipline.md.");
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_NODE_PERSIST_FIELD]
//======================================================================

//----------------------------------------------------------------------
// [MACRO]_[NPF_PROJECT_SAVE]
// [TAG]_[[ENGINE] [PERSISTENCE] [FRAMEWORK_DISCIPLINE]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the 3 projection views — SAVE (ctx→wire) / READ (wire→NodeSnap staging, dispatches on STORAGE_KIND ONLY) / COMMIT (staging→ctx, dispatches COMMIT_KIND x STORAGE_KIND; undesigned combos static_assert(false))]
//----------------------------------------------------------------------
// Caller-scope contracts (the OMS precedent, OmsFieldRegistry.hpp:570-580):
//   SAVE   — inside a `template <unsigned F>` fn; scope has `const NodeContext<F>& ctx`,
//            `FILE* f`, and a `fail:` label. Expansion site: ShardedSnapshot_Save per-node loop.
//   READ   — inside a `template <unsigned F>` fn; scope has `NodeSnap& s`, `FILE* f`;
//            enclosing fn returns 0 on fread failure. NodeSnap field names == registry
//            NAMEs (the struct IS the DECLARE view, hand-kept — nested staging landed
//            at Step 2c and is NOT regenerated here).
//   COMMIT — scope has `NodeSnap& s` and `NodeContext<F>& ctx`.
//
// ⚠ THE A2 INVARIANT (adversarial pass, 2026-07-04): READ dispatches on STORAGE_KIND
// ONLY — NEVER on COMMIT_KIND. A NO_COMMIT row still consumes its wire bytes; gating
// READ on COMMIT_KIND (the naive OMS SKIP_PERSIST mirror) desyncs every later offset
// by 2 bytes per node block. NO_COMMIT no-ops ONLY at the COMMIT projection.

// ---- SAVE (ctx → wire) ----
#define NPF_PROJECT_SAVE(NAME, TYPE, SKIND, SMASK, CKIND) \
    NPF_SAVE_##SKIND(NAME, TYPE, SMASK)

#define NPF_SAVE_SCALAR(NAME, TYPE, SMASK) \
    if (fwrite(&ctx.NAME, sizeof(TYPE), 1, f) != 1) goto fail;
// BIT: extract the node_state_flags bit → 1-byte 0/1 wire byte (bytewise-identical
// to the pre-.B.3 uint8_t field format — no version bump was needed at the migration).
#define NPF_SAVE_BIT(NAME, TYPE, SMASK)                                                    \
    {                                                                                      \
        TYPE _npf_b = NODE_STATE_FLAG_IS_SET(ctx, SMASK) ? (TYPE)1 : (TYPE)0;              \
        if (fwrite(&_npf_b, sizeof(TYPE), 1, f) != 1) goto fail;                           \
    }
// PAD: SMASK zero bytes (wire-identical to the retired hand-written pads).
#define NPF_SAVE_PAD(NAME, TYPE, SMASK)                                                    \
    {                                                                                      \
        TYPE _npf_z[SMASK] = {};                                                           \
        if (fwrite(_npf_z, sizeof(TYPE), SMASK, f) != (size_t)(SMASK)) goto fail;          \
    }
// DELEGATE: the sub-registry walker AT this wire position (field-by-field, never a
// struct blob — blob diverges: regime 48B struct vs 36B wire; feeder 144 vs 136).
#define NPF_SAVE_DELEGATE(NAME, TYPE, SMASK) \
    if (SMASK##_FieldwiseWrite(&ctx.NAME, f) != 0) goto fail;

// ---- READ (wire → NodeSnap staging; STORAGE_KIND dispatch ONLY — see A2 above) ----
#define NPF_PROJECT_READ(NAME, TYPE, SKIND, SMASK, CKIND) \
    NPF_READ_##SKIND(NAME, TYPE, SMASK)

#define NPF_READ_SCALAR(NAME, TYPE, SMASK) \
    if (fread(&s.NAME, sizeof(TYPE), 1, f) != 1) { fclose(f); return 0; }
#define NPF_READ_BIT(NAME, TYPE, SMASK) \
    if (fread(&s.NAME, sizeof(TYPE), 1, f) != 1) { fclose(f); return 0; }
#define NPF_READ_PAD(NAME, TYPE, SMASK)                                                    \
    {                                                                                      \
        TYPE _npf_z[SMASK];                                                                \
        if (fread(_npf_z, sizeof(TYPE), SMASK, f) != (size_t)(SMASK)) { fclose(f); return 0; } \
    }
#define NPF_READ_DELEGATE(NAME, TYPE, SMASK) \
    if (SMASK##_FieldwiseRead(&s.NAME, f) != 0) { fclose(f); return 0; }

// ---- COMMIT (staging → ctx; COMMIT_KIND × STORAGE_KIND) ----
#define NPF_PROJECT_COMMIT(NAME, TYPE, SKIND, SMASK, CKIND) \
    NPF_COMMIT_##CKIND##_##SKIND(NAME, TYPE, SMASK)

#define NPF_COMMIT_COMMIT_SCALAR(NAME, TYPE, SMASK)  ctx.NAME = s.NAME;
#define NPF_COMMIT_COMMIT_BIT(NAME, TYPE, SMASK)                                           \
    do {                                                                                   \
        if (s.NAME) { NODE_STATE_FLAG_SET(ctx, SMASK); }                                   \
        else        { NODE_STATE_FLAG_CLR(ctx, SMASK); }                                   \
    } while (0);
#define NPF_COMMIT_COMMIT_DELEGATE(NAME, TYPE, SMASK) \
    SMASK##_CommitPersistedFields(&ctx.NAME, &s.NAME);
// NO_COMMIT: read-but-not-applied (the v5.11.15 leak fields) — silence unused staging.
#define NPF_COMMIT_NO_COMMIT_SCALAR(NAME, TYPE, SMASK)  (void)s.NAME;
#define NPF_COMMIT_NO_COMMIT_PAD(NAME, TYPE, SMASK)
// Undesigned combos red-build until a first user designs them (OMS precedent):
#define NPF_COMMIT_COMMIT_PAD(NAME, TYPE, SMASK)                                           \
    static_assert(false, "COMMIT + PAD is meaningless — pad bytes carry no state. "        \
                         "Mark the pad row NO_COMMIT.");
#define NPF_COMMIT_NO_COMMIT_BIT(NAME, TYPE, SMASK)                                        \
    static_assert(false, "NO_COMMIT + BIT not yet designed in NodeCtxPersistRegistry.hpp " \
                         "— first user decides whether the staged byte needs a (void) "    \
                         "consume or a diagnostic. See the OMS static_assert precedent.");
#define NPF_COMMIT_NO_COMMIT_DELEGATE(NAME, TYPE, SMASK)                                   \
    static_assert(false, "NO_COMMIT + DELEGATE not yet designed — a read-but-unapplied "   \
                         "sub-block wants an explicit design (offset-consume semantics "   \
                         "are the delegate's own). See the OMS static_assert precedent.");

#endif  // NODE_CTX_PERSIST_REGISTRY_HPP
