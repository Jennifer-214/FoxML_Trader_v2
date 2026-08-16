// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[MemHeaders/NodeCtxPersistRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [FRAMEWORK_DISCIPLINE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ordered per-node persist WIRE SPEC — 29 rows in exact fwrite-call order (SCALAR/BIT/PAD/DELEGATE x COMMIT/NO_COMMIT); SAVE/READ/COMMIT projections auto-generate the serializer walk; plus the COMPLEMENT registry naming the 22 deliberately-unpersisted members]
// [CONTAINS]
//   - [REGISTRY]_[FOREACH_NODE_PERSIST_FIELD]
//   - [REGISTRY]_[FOREACH_NODE_CTX_PERSIST_EXEMPT]
//   - [MACRO]_[NPF_PROJECT_SAVE]   (+ READ + COMMIT projections)
// [REFERENCE]_[DECISION]_[[D-291] [D-302] [D-305] [D-421]]
// [REFERENCE]_[INVARIANT]_[[H9] [H15] [H21]]
// [REFERENCE]_[CLASS]_[18]
// [REFERENCE]_[DESIGN_SPEC]_[[registry-tuple-as-single-source-of-truth] [wire-format-byte-preservation-discipline] [autopopulate-pattern-for-production-caller-class]]
//======================================================================================================
// E.1.2 D-305 tail (the hybrid flat-registry + delegates shape locked by D-291/BLK-1,
// forcing-function architecture locked by D-302): ONE ordered registry replaces the
// hand-written per-node fwrite/fread/commit loops in ShardedSnapshotPersist.hpp.
// Dropping a wire op is now dropping a ROW — caught by the count-lock + the frozen
// byte-golden (tests/sharded_snapshot_v11_golden.hpp), closing the
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
// [OVERVIEW]_[29 wire rows, 1944B per node block at snapshot v11 — 20 SCALAR + 1 BIT + 2 PAD + 3 DELEGATE + the 3 interleave doubles; tuple X(NAME, TYPE, STORAGE_KIND, STORAGE_MASK, COMMIT_KIND)]
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
// leaked the strategy_state pointer. Both STAY on the wire — they READ (consuming
// their bytes keeps every later offset correct) and no-op at commit.
//
// v5.4.3 (snapshot v5) history rides node_gross_wins/losses + idle_cycles (Class 4:
// added v4.7.25, silently never persisted — Stats avg_win/avg_loss/profit_factor read
// $0.00 after restart). THE founding instance of the silent-field-drop class this
// registry structurally closes.
#define FOREACH_NODE_PERSIST_FIELD(X)                                                                \
    /* identity / sizing */                                                                          \
    X(strategy_id,            uint8_t,              SCALAR,   0,                  NO_COMMIT)          \
    X(resolved_strategy_id,   uint8_t,              SCALAR,   0,                  COMMIT)             \
    X(strategy_state_kind,    uint8_t,              SCALAR,   0,                  NO_COMMIT)          \
    X(_pad_ids,               uint8_t,              PAD,      1,                  NO_COMMIT)          \
    X(allocated_balance,      Money,                SCALAR,   0,                  COMMIT)             \
    /* counters + W/L stats (partner_pending_pnl = the AM-4/TD-227 pairing park: first-leg    */     \
    /* net parked here until the partner exits, then merged into ONE W/L stat. Persisted at   */     \
    /* v11 (D-420); its partner_pending_bitmap is EventLoopState-level and RE-DERIVED on load */     \
    /* via slot parity — never a wire row.)                                                   */     \
    X(entries_processed,      uint64_t,             SCALAR,   0,                  COMMIT)             \
    X(exits_processed,        uint64_t,             SCALAR,   0,                  COMMIT)             \
    X(node_realized,          Money,                SCALAR,   0,                  COMMIT)             \
    X(node_fees,              Money,                SCALAR,   0,                  COMMIT)             \
    X(node_open_notional,     Money,                SCALAR,   0,                  COMMIT)             \
    X(node_wins,              uint32_t,             SCALAR,   0,                  COMMIT)             \
    X(node_losses,            uint32_t,             SCALAR,   0,                  COMMIT)             \
    X(node_gross_wins,        Money,                SCALAR,   0,                  COMMIT)             \
    X(node_gross_losses,      Money,                SCALAR,   0,                  COMMIT)             \
    X(partner_pending_pnl,    Money,                SCALAR,   0,                  COMMIT)             \
    X(idle_cycles,            uint32_t,             SCALAR,   0,                  COMMIT)             \
    /* spacing state */                                                                              \
    X(last_entry_price,       Money,                SCALAR,   0,                  COMMIT)             \
    X(last_entry_tick,        uint64_t,             SCALAR,   0,                  COMMIT)             \
    X(sl_cooldown_remaining,  uint32_t,             SCALAR,   0,                  COMMIT)             \
    /* kill switch — kill bit wire-encoded as a 1-byte 0/1 (pre-.B.3 format preserved).       */     \
    /* node_dd_pct DROPPED at v11 (D-420): eval-transient. NARROWED at D-421 after the reads  */     \
    /* were enumerated — the old wording said "recomputed before EVERY read", and it covers   */     \
    /* 2 of the 4. Both CAPITAL reads (the kill-trip eval + its log) ARE dominated by the      */    \
    /* recompute from the persisted node_peak_balance, in the same pass, both branches         */    \
    /* assigning — so no capital decision ever sees a stale drawdown, which is the property    */    \
    /* the drop rests on. The other two reads (the TUI publish + the paper-reset summary emit) */     \
    /* are OUT of pass and display 0 for one slow-path cycle after a warm restart. Accepted,   */    \
    /* display-only, and now stated rather than implied.                                       */    \
    X(node_peak_balance,      Money,                SCALAR,   0,                  COMMIT)             \
    X(node_kill_tripped,      uint8_t,              BIT,      KILL_TRIPPED,       COMMIT)             \
    X(_pad_kill,              uint8_t,              PAD,      3,                  NO_COMMIT)          \
    X(node_ks_trips_total,    uint32_t,             SCALAR,   0,                  COMMIT)             \
    /* sub-struct delegates at their wire positions (E.1.2 Step-2c, D-304) */                        \
    X(regime_state,           RegimeState<F>,       DELEGATE, RegimeState,        COMMIT)             \
    X(pnl_feeder,             RegressionFeederX<F>, DELEGATE, RegressionFeederX,  COMMIT)             \
    /* the D-110 interleave: 3 confidence doubles BETWEEN feeder and confidence */                    \
    X(staged_prediction,      double,               SCALAR,   0,                  COMMIT)             \
    X(active_prediction,      double,               SCALAR,   0,                  COMMIT)             \
    X(last_confidence,        double,               SCALAR,   0,                  COMMIT)             \
    /* confidence delegate (FOREACH_CONFIDENCE_PERSIST_FIELD, 1552B) */                              \
    X(confidence,             ConfidenceScorer,     DELEGATE, ConfidenceScorer,   COMMIT)

//------------------------------------------------------------------
// [SECTION]_[COMPILE-TIME COUNT SENTINEL — the parent count-lock]
//------------------------------------------------------------------
// Wire pin: EXACTLY 29 rows at snapshot v11 (the v10→v11 delta was the net-0 row
// swap node_dd_pct → partner_pending_pnl — the count-lock is VACUOUS against
// exactly that class; the name-listing golden is the layer that catches it).
// Adding/removing/reordering a row CHANGES the per-node wire →
// SHARDED_SNAPSHOT_VERSION bump + golden regen (regen/RENAME
// tests/sharded_snapshot_v11_golden.hpp) ride the SAME commit (H21).
// Sub-registry tripwires (regime ==7 · feeder ==3 · confidence ==7) live beside
// their registries — a delegate-internal drop is caught THERE; this lock catches
// parent-level row motion.
#define _NPF_COUNT_ONE(NAME, TYPE, SKIND, SMASK, CKIND) +1
constexpr int FOREACH_NODE_PERSIST_FIELD_COUNT =
    0 FOREACH_NODE_PERSIST_FIELD(_NPF_COUNT_ONE);
#undef _NPF_COUNT_ONE
// [ASSERT]_[REGISTRY_COVERAGE]_[FOREACH_NODE_PERSIST_FIELD_COUNT == 29 — the per-node wire row pin]
static_assert(FOREACH_NODE_PERSIST_FIELD_COUNT == 29,
              "Snapshot v11 per-node wire = EXACTLY 29 ordered rows (1944B/node). "
              "Adding/removing/reordering a row changes the wire format: bump "
              "SHARDED_SNAPSHOT_VERSION + regen/RENAME the version-named golden "
              "(tests/sharded_snapshot_v11_golden.hpp) in the SAME commit. See "
              "DESIGN_SPECS/wire-format-patterns/wire-format-byte-preservation-discipline.md.");
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_NODE_PERSIST_FIELD]
//======================================================================

//======================================================================
// [REGISTRY]_[FOREACH_NODE_CTX_PERSIST_EXEMPT]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [PERSISTENCE] [FRAMEWORK_DISCIPLINE] [DETERMINISM]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the COMPLEMENT half of the NodeContext partition — the 22 members deliberately NOT on the wire, each with a falsifiable category + a rationale a reviewer can refute; tuple X(NAME, CATEGORY, "rationale")]
// [COLUMN]_[NAME]_[NodeContext<F> member name, exactly as clang reports it — a name that stops being a member is STALE-EXEMPT, not silently ignored]
// [COLUMN]_[CATEGORY]_[a closed set declared in tools/check_node_ctx_partition.py CATEGORIES; the token is matched EXACTLY (no parentheses, no arguments — cites belong in the rationale)]
// [COLUMN]_[rationale]_[what a reviewer should go VERIFY; must be falsifiable from code, never "it is transient"]
// [REFERENCE]_[DECISION]_[[D-305] [D-421]]
// [REFERENCE]_[INVARIANT]_[[H15] [H21] [H22]]
// [REFERENCE]_[CLASS]_[[4] [30]]
//======================================================================
// WHY THIS EXISTS — producer-side COMPLEMENT BLINDNESS, the class D-421 codifies.
// (Deliberately NOT tagged with a catalog Class id: that entry does not exist yet — it
// lands at D-421 step 4, WITH this guard, so it can be written from three instances and
// a sharpened signature instead of one anecdote. Referencing an id before it exists is
// the phantom-reference shape TECH_DEBT-274 tracks, and the tag validator RED-ed on
// exactly that when this row first cited it. Add the [CLASS] tag here at step 4.)
// FOREACH_NODE_PERSIST_FIELD is a COVERAGE registry over a HAND-declared struct
// (NodeContext<F>, ControllerEventLoop.hpp:315). Every guard around it points the rows
// FORWARD — the ==29 count-lock, the 46-row flattened layout golden, the frozen byte
// golden, the paired-bump rule — so all four answer "are the rows we have right?" and
// NONE answers "are these ALL the rows there should be?". A member added and never
// enrolled is invisible to every one of them. That is not hypothetical:
//
//   - node_gross_wins / node_losses / idle_cycles (v4.7.25): added, never persisted.
//     Stats read $0.00 after every restart until v5.4.3. TECH_DEBT-196.
//   - ic.actuals.{count,head} (2026-08-15): unpersisted while its SIBLING
//     ic.predictions.{count,head} was persisted. A perfectly-correlated predictor read
//     IC = -0.5238 after a warm restart, and that IC drives an auto-kill capital
//     control. It carried a STATED reason — "the two rings advance in lockstep" — that
//     was true of the push path and FALSE across the persist boundary. Fixed at 564f099.
//
// That second one is why a row is a CATEGORY and not a checkbox: a reason merely written
// down is a hypothesis. The categories are phrased so a reviewer knows what evidence
// would refute each ("DERIVED_EACH_PASS" says: find me the unconditional write, and tell
// me what reads it before the first one). The guard checks membership + staleness +
// contradiction; it deliberately does NOT judge whether a rationale is TRUE, because
// mechanizing that would manufacture exactly the false confidence ic.actuals is made of.
//
// SUBTRACTION IS THE POINT: clang's real member list MINUS the persist rows MINUS these
// rows must be EMPTY. Adding a NodeContext member with neither row = UNACCOUNTED = RED.
// Guard: tools/check_node_ctx_partition.py (rc 0/1/2; --selftest, 17 teeth).
//
// EVERY ROW BELOW WAS VERIFIED FIELD-BY-FIELD, not assigned by inspection — the P1/P2/P3
// i-class pass of 2026-08-15 (frozen at plans/<sprint>/reports/2026-08-15-nodectx-
// exemption-verification/). That pass found THREE live engine defects and REFUTED two
// categories that had sounded reasonable, which is the whole argument for the exercise.
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_NODE_CTX_PERSIST_EXEMPT(X)                                                           \
    /* --- handles + lifecycle (P1) ------------------------------------------------------ */       \
    /* The pointers are re-established at boot BEFORE the producer spawns (Run.hpp:1358),   */       \
    /* so the whole family is safe for the same structural reason. RUNTIME_POINTER carries  */       \
    /* the re-establishment SITE because "it is a pointer" is a property of the declaration,*/       \
    /* not evidence about the runtime.                                                      */       \
    X(gate_state,        DERIVED_EACH_PASS,                                                          \
      "Assigned WHOLE (flags = _new_flags, SlowPathGateRegistry.hpp:234) by "                        \
      "SLOW_PATH_GATE_AUTOPOPULATE_PER_NODE at ControllerEventLoop.hpp:2695 every rebuild, "         \
      "guarded only on slot bounds. What reads it before the FIRST derive: the producer "            \
      "thread at ShardedSnapshot.hpp:597 (MASK_LADDER_ACTIVE -> a PerNodeSnap bit) -- an "           \
      "unsynchronized cross-thread read. That read was INDETERMINATE until the NSDMI at "            \
      "SlowPathGateRegistry.hpp:196 (D-421); it now yields a defined 0, i.e. ladder-inactive, "      \
      "which is the correct pre-first-pass projection. Refute by removing the NSDMI, or by "         \
      "finding a reader of gate_state.flags that is NOT ShardedSnapshot.hpp:597 or one of the "      \
      "six same-pass ML dispatch reads in StrategyParameters.hpp (:1203 :1218 :1436 :1445 "          \
      ":1605 :1614), all of which run AFTER :2695 in the same rebuild.")                             \
    X(core,              RUNTIME_POINTER,                                                            \
      "ExecutionCore<F>* re-established at NodeCtxInitRegistry.hpp:100 (nullptr) then "              \
      "ControllerEventLoop.hpp:1285 (inside EventLoopState_RegisterCore, :1269), both before the "   \
      "producer spawn at EngineSharded/Run.hpp:1358. Pointee is the hot-path core, itself "          \
      "boot-constructed.")                                                                           \
    X(slow_state,        RUNTIME_POINTER,                                                            \
      "NodeSlowState<F>* arena-allocated at NodeCtxInitRegistry.hpp:322 (Layer 4, "                  \
      "_alloc_and_init_slow_state) before the producer spawn. The rolling/regime state behind "      \
      "it is warm-up-rebuilt by the owning slow thread, not restored.")                              \
    X(model_handle,      RUNTIME_POINTER,                                                            \
      "void* re-established at NodeCtxInitRegistry.hpp:101 (nullptr) then EngineCommon.hpp:343 "     \
      "(per-node model load from cfg core_N_model_path/_dir). A restored pointer value would "       \
      "be a dangling address from the previous process -- restoring it would be WRONG.")             \
    X(ensemble_handle,   RUNTIME_POINTER,                                                            \
      "void* re-established at NodeCtxInitRegistry.hpp:102 (nullptr) then EngineCommon.hpp:394 "     \
      "(zoo load). Same dangling-address argument as model_handle.")                                 \
    X(strategy_state,    POINTEE_STATE_REDERIVED,                                                    \
      "RUNTIME_POINTER alone is TRUE BUT INSUFFICIENT here and the distinction is the point: "       \
      "the pointer is re-established at NodeCtxInitRegistry.hpp:103 + EngineCommon.hpp (per-node "   \
      "Strategy_InitPerCore), but ~720B of ACCUMULATED strategy adaptation lives BEHIND it. That "   \
      "pointee state is re-derived by warm-up, not restored -- which is the claim to verify, and "   \
      "the near-miss of the ic.actuals shape (P1 4.6). Refute by finding pointee state that "        \
      "neither warm-up nor cfg re-establishes.")                                                     \
    X(pending_params,    DERIVED_EACH_PASS,                                                          \
      "GateParameters<F> boot-initialized at NodeCtxInitRegistry.hpp:308 (Layer 2, "                 \
      "GateParameters_Init) and rewritten every rebuild. What reads it before the first derive: "    \
      "nothing can -- a DIRTY gate structurally prevents pre-derive consumption, which is why "      \
      "this is the strongest case of the seven (P1 4.5) and gate_state above is not.")               \
    /* --- eval-cycle transients + display sinks (P2) -------------------------------------- */      \
    /* The three intended_* are DERIVED_BEFORE_ARM, not DERIVED_EACH_PASS: their capital     */      \
    /* consumer is unreachable until an INDEPENDENT arming flag opens, and that flag cannot  */      \
    /* open before the first rebuild. Named flag + init + sole grant site, per the category. */      \
    X(intended_tp,       DERIVED_BEFORE_ARM,                                                         \
      "Written ControllerEventLoop.hpp:3501 every full rebuild. The arming flag is the core's "      \
      "PERMISSION bit: the only capital consumer (handle_buy_fill) is unreachable until it is 1, "   \
      "and its ONLY grant site is ExecutionCore_SetPermission(.., 1) at EngineCommon.hpp:809, "      \
      "which runs after the boot rebuild. The two other writers are boot/plumbing, not competing "   \
      "derives: :1286 seeds it inside EventLoopState_RegisterCore and :1608 is the "                 \
      "EventLoopState_SetIntendedParams setter. Note TECH_DEBT-276: these three are also "           \
      "unenumerated cross-thread multi-word reads -- a separate concern from persistence, "          \
      "tracked, not silently folded in here.")                                                       \
    X(intended_sl,       DERIVED_BEFORE_ARM,                                                         \
      "Written ControllerEventLoop.hpp:3502 every full rebuild; same permission arming flag "        \
      "(EngineCommon.hpp:809) and same two boot/setter writers (:1287, :1609) as intended_tp. "      \
      "TECH_DEBT-276 applies.")                                                                      \
    X(intended_qty,      DERIVED_BEFORE_ARM,                                                         \
      "Written ControllerEventLoop.hpp:3503 every full rebuild; same permission arming flag "        \
      "(EngineCommon.hpp:809) and same two boot/setter writers (:1288, :1610) as intended_tp. "      \
      "TECH_DEBT-276 applies.")                                                                      \
    X(halt_reason,       DERIVED_EACH_PASS,                                                          \
      "Unconditional = HALT_OK at ControllerEventLoop.hpp:3156, DOMINATING the first-reason-wins "   \
      "reads at :3218/:3219 and :3232/:3233 and the log read at :3519. Its reset deliberately "      \
      "sits ABOVE those consumers but BELOW strategy_halt_reason's -- the two look "                 \
      "interchangeable and are NOT, which is why tools/check_reset_before_producer.py pins each "    \
      "direction separately rather than asserting one rule for both.")                               \
    X(strategy_halt_reason, DERIVED_EACH_PASS,                                                       \
      "Unconditional = SHALT_OK at ControllerEventLoop.hpp:3082. This became TRUE only at "          \
      "D-421: the reset had sat 59 lines BELOW its producer since bc37c62 (2026-04-30), so 17 of "   \
      "20 SHALT codes were clobbered before anyone could observe them -- derived-each-pass into a "  \
      "value nobody could read. The hoist is why :3082 now sits ABOVE halt_reason's :3156; "         \
      "tools/check_reset_before_producer.py pins the ordering (Class 44 sub-B).")                    \
    X(last_confidence_factor, DISPLAY_SINK_ONLY,                                                     \
      "EVERY reader enumerated: ShardedSnapshot.hpp:591 and :598, and nowhere else tree-wide. "      \
      "Written StrategyParameters.hpp:1717. Explicitly NOT an execution input -- ML sizing uses "    \
      "the function-local factor at StrategyParameters.hpp:1743; the NodeContext field is never "    \
      "read back into an execution path (P2 refuted the sister report's claim that it was).")        \
    X(last_exit_prediction, DERIVED_EACH_PASS,                                                       \
      "Reset = 0.0 at ControllerEventLoop.hpp:3029, before the read at EngineCommon.hpp:669 in "     \
      "the same slow-path body. Deliberately NOT DISPLAY_SINK_ONLY: it LOOKS like an ML "            \
      "observability field and rides PerNodeSnap.ml_last_exit_prediction, but :669 reads it as "     \
      "the predicate that fires a real OMS_PushExitForSlot MARKET_SELL. 'It is just for display' "   \
      "is the sentence that hides a live one.")                                                      \
    X(last_exit_dominant_horizon, DERIVED_EACH_PASS,                                                 \
      "Reset = -1 at ControllerEventLoop.hpp:3030, ahead of any consumer in the same pass.")         \
    X(last_buy_dominant_horizon, DERIVED_EACH_PASS,                                                  \
      "Reset = -1 at ControllerEventLoop.hpp:3036, ahead of any consumer in the same pass.")         \
    X(last_barrier_mode_used, DERIVED_EACH_PASS,                                                     \
      "Reset = 0 at ControllerEventLoop.hpp:3037, ahead of any consumer in the same pass.")          \
    X(last_entry_wall_us, SUPERSEDED_BY_PERSISTED_SIBLING,                                           \
      "The sibling is Position::entry_timestamp_us, which IS persisted (raw Position dump, "         \
      "ShardedSnapshotPersist.hpp:191); the fallback site is ShardedSnapshot.hpp:286-291. "          \
      "NOT a wall-clock-is-meaningless argument -- that reading is exactly BACKWARDS here and "      \
      "the codebase proves it: losing this value WAS a real bug (Hold column read '0m' forever "     \
      "for restored positions) and v5.11.65 fixed it by persisting the SIBLING, not by "             \
      "declaring the field transient.")                                                              \
    X(node_dd_pct,       DERIVED_EACH_PASS,                                                          \
      "Written at ControllerEventLoop.hpp:3320 and :3323 (BOTH arms of the drop test), read at "     \
      ":3334 and :3338 in the same block with no control flow between -- so the KILL-SWITCH "        \
      "evaluation is sample-fresh. Scope honestly bounded: the 'recomputed before every read' "      \
      "claim covers 2 of 4 readers; the DISPLAY read is stale for one slow-path cycle after a "      \
      "warm restart, which is accepted and stated rather than implied (P2 B.1 corrects D-420's "     \
      "wording, and the quantifier is the exact shape M9 exists to catch).")                         \
    /* --- unpersisted sub-structs (P3) ---------------------------------------------------- */     \
    X(turnover,          DISPLAY_SINK_ONLY,                                                          \
      "EVERY consumer enumerated: the single projection at ShardedSnapshot.hpp:622 into "            \
      "PerNodeSnap.ml_portfolio_turnover (declared EngineTUI.hpp:1208), which has NO renderer "      \
      "tree-wide -- the diagnostic chain is dead below the snapshot (P3 B-2). Enumerated rather "    \
      "than asserted precisely so this ROTS LOUDLY: adding a render row makes the claim false and "  \
      "the exemption re-earnable, instead of quietly wrong.")                                        \
    X(drift_history,     ACCEPTED_RESET,                                                             \
      "Operator-accepted 2026-08-15 (option (a), with the option (d) honest-abstain queued). "       \
      "WHAT DEGRADES AND FOR HOW LONG: DriftHistory_CheckBreach returns 0 for the first 4 closed "   \
      "ML trades after a restart (ConfidenceScore.hpp:1397/:1410 both require >= 5 samples), and "   \
      "returns the SAME 0 it returns for healthy. Bounded by four VERIFIED mechanisms: (1) the "     \
      "capital OUTPUT persists -- node_kill_tripped is wire row 020, so a drift-killed node stays "  \
      "killed; (2) breach => kill is same-call (ControllerEventLoop.hpp:1903 -> :1916), so no "      \
      "durable breaching-but-not-killed state exists to lose; (3) the detector INPUT is warm -- "    \
      "RollingIC persists at wire rows 039-042 and is lockstep-restored at ConfidenceScore.hpp:1485 "\
      "(the D-421 step-1 fix), so the FIRST post-restart sample carries the full pre-restart IC; "   \
      "(4) the verdict is a MEAN, so halving the sample count does not move it (probed: continuous " \
      "avg=-0.3000 n=40 vs restarted avg=-0.3000 n=20). Under the shipping default "                 \
      "auto_kill_on_drift=0 (ControllerConfig.hpp:2127) the cost is 4 trades of a missing log line " \
      "and badge. NOTE the inversion this row used to rest on is GONE: being unpersisted was once "  \
      "the ONLY thing re-arming the drift auto-kill, and D-421 replaced that accident with explicit "\
      "clears at Async.hpp:412 (manual reset, latch only) and NodeCtxInitRegistry.hpp:345 (paper "   \
      "reset, full Init). Persisting the ring is still REJECTED, now on cost alone: +4160B/node "    \
      "takes the per-node wire 1944 -> 6104B (3.1x) to buy 4 trades.")                               \
    X(sp_telemetry,      DISPLAY_SINK_ONLY,                                                          \
      "EVERY reader enumerated: EngineTUI.hpp:2060 (last_tick_us), :2062 (cycles_total), :2064 "     \
      "(yield_count), :2066 (state) -- four relaxed loads in the TUI render, and nothing else "      \
      "tree-wide. Writers are the slow-path threads (EngineCommon.hpp:825/:827, "                    \
      "EngineSharded/Run.hpp:1747-1769) and Init (NodeCtxInitRegistry.hpp:317-320). Persisting "     \
      "would be ACTIVELY HARMFUL, not merely useless: a restored live thread-state byte renders a "  \
      "running node as PAUSED and a restored cycle counter renders a fresh node as stalled.")

//------------------------------------------------------------------
// [SECTION]_[COMPILE-TIME COUNT SENTINEL — the complement count-lock]
//------------------------------------------------------------------
// 22 = sizeof(NodeContext<64>) member count (49, clang-derived) MINUS the 27 members the
// persist registry covers. This lock is a TRIPWIRE, not the guard: it catches a row
// deleted by hand, and is BLIND by construction to the case that matters (a NEW struct
// member with no row anywhere) — that one is only visible by subtracting from clang's
// real member list, which is what check_node_ctx_partition.py does. Stated explicitly
// because a count-lock reads like coverage and is not (the ==29 sibling above is vacuous
// against exactly the net-0 row swap that shipped at v11).
#define _NPE_COUNT_ONE(NAME, CATEGORY, WHY) +1
constexpr int FOREACH_NODE_CTX_PERSIST_EXEMPT_COUNT =
    0 FOREACH_NODE_CTX_PERSIST_EXEMPT(_NPE_COUNT_ONE);
#undef _NPE_COUNT_ONE
// [ASSERT]_[REGISTRY_COVERAGE]_[FOREACH_NODE_CTX_PERSIST_EXEMPT_COUNT == 22 — the complement pin]
static_assert(FOREACH_NODE_CTX_PERSIST_EXEMPT_COUNT == 22,
              "NodeContext<64> has 49 members; 27 are covered by FOREACH_NODE_PERSIST_FIELD, so "
              "EXACTLY 22 are declared-unpersisted here. Adding a NodeContext member means adding "
              "it to ONE of the two registries — run tools/check_node_ctx_partition.py, which "
              "subtracts both from clang's real member list and REDs on UNACCOUNTED / STALE-EXEMPT "
              "/ CONTRADICTION. See DESIGN_SPECS/framework-patterns/"
              "registry-coverage-ci-check-pattern.md.");
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_NODE_CTX_PERSIST_EXEMPT]
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
