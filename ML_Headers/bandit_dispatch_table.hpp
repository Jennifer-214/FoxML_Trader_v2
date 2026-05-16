// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [BANDIT DISPATCH TABLE — v5.15.5.F.4d]
//======================================================================================================
// Pattern 1 fn-pointer dispatch tables for REWARD ATTRIBUTION (separate from
// decision-time arm-selection in BanditAlgorithmRegistry.hpp). Auto-derived
// from FOREACH_BANDIT_ALGORITHM metadata per pattern progression at v5.15.5.F.4d:
//
//   - multi-state-dispatch-with-per-state-update-metadata.md Stage 3 ACTIVE
//     first canonical: row-local metadata (`exp3_up`, `thompson_up`, `drives`)
//     drives auto-derivation of reward dispatch tables. Adding a 6th bandit
//     algorithm = 1 row in FOREACH_BANDIT_ALGORITHM with metadata tuple →
//     both reward dispatch tables (buy + exit) auto-extend.
//
//   - branchless-dispatch-discipline.md Pattern 1 (auto-derived fn-pointer
//     dispatch table). Per H20 + Class 28 — branchless reward dispatch
//     replaces the legacy switch-on-bandit-algorithm in caller sites.
//
//   - sink-fn-pointer-for-optional-side-effect-pattern.md (Pattern 5):
//     `ezoo->thompson_update_fn` / `ezoo->exit_thompson_update_fn` resolve to
//     `noop_thompson_update` when subsystem not enabled, `real_thompson_update`
//     when enabled (boot-wired). Dispatch tables call through the sink-fn-pointer
//     unconditionally; per-arm-update sites have no `if (thompson_active)` branch.
//
// FILE ORDERING (per Step 0.C of .F.4d merged plan body):
//   1. EnsembleModelZoo<F> + Thompson_Update + Bandit_Update defined BEFORE this header
//   2. FOREACH_BANDIT_ALGORITHM (7-arg shape post-.F.4d Step 2.B) defined BEFORE this header
//   3. This header included by callers AFTER step (2)
//   4. Boot wiring (`ezoo->thompson_update_fn = &real_thompson_update`) happens AT BOOT
//      AFTER ezoo is initialized
//
// CURRENT STATUS at file creation (v5.15.5.F.4d Step 1.C):
//   - FOREACH_BANDIT_SIDE meta-X-macro: DEFINED (this Step)
//   - Bit-width static_asserts for Order::flags_packed bandit context bits: DEFINED (this Step)
//   - g_buy_reward_dispatch / g_exit_reward_dispatch tables: DEFERRED to Step 1.B
//     (depends on Step 2.B's 7-arg FOREACH_BANDIT_ALGORITHM expansion +
//     Step 1.D's ezoo->thompson_update_fn field)
//
//======================================================================================================
#ifndef BANDIT_DISPATCH_TABLE_HPP
#define BANDIT_DISPATCH_TABLE_HPP

#include "BanditAlgorithmRegistry.hpp"      // FOREACH_BANDIT_ALGORITHM + FOREACH_BANDIT_ALGORITHM_COUNT
#include "CoreModelZoo.hpp"                  // EnsembleModelZoo<F> + ENSEMBLE_HORIZON_MAX
#include "../Strategies/StrategyInterface.hpp"   // NUM_REGIMES
#include "../CoreFrameworks/Order.hpp"       // MASK_ORDER_BANDIT_3BIT (referenced by bit-cap static_asserts below)

//======================================================================================================
// [FOREACH_BANDIT_SIDE — meta-X-macro for buy/exit symmetry]
//======================================================================================================
// Per § G.1 of v5.15.5.F.4d merged plan body. Closes Class 18 (mirror-incomplete)
// at the buy-side/exit-side bandit boundary structurally.
//
// 2-row registry: buy + exit. Adding a future side (per-symbol? per-strategy?) = 1 row
// here → all consumer-site mirrors auto-extend:
//   - g_<side>_reward_dispatch<F>[N] dispatch tables (per Step 1.B)
//   - ezoo-><side>_thompson_update_fn Pattern 5 sink fields (per Step 1.D)
//   - MASK_EZOO_<SIDE>_THOMPSON_READY init flags (per § G.3 — FOREACH_EZOO_INIT_FLAG)
//   - EnsembleModelZoo_Init<Side>ThompsonBandits init fns (per Step 1.D — CoreModelZoo.hpp)
//   - EnsembleModelZoo_Load<Side>ThompsonState / _Save<Side>ThompsonState (per Step 1.D)
//   - FOREACH_ENSEMBLE_POST_LOAD rows for init + load (per Step 1.E)
//
// At v5.15.5.F.4d ship close: this is the FIRST CANONICAL of FOREACH_BANDIT_SIDE.
// Future extension (per-symbol axis post-.F.4e KIND_STRING infrastructure) = 1-row add.
//======================================================================================================
#define FOREACH_BANDIT_SIDE(X) \
    X(buy) \
    X(exit)

// Compile-time count for tests / static_asserts.
#define _BANDIT_SIDE_COUNT_ONE(side) +1
#define FOREACH_BANDIT_SIDE_COUNT (0 FOREACH_BANDIT_SIDE(_BANDIT_SIDE_COUNT_ONE))

//======================================================================================================
// [BIT-WIDTH INVARIANTS — Order::flags_packed bandit context bits 17-25]
//======================================================================================================
// Per § N.1 of v5.15.5.F.4d merged plan body. SHIFT_ORDER_BANDIT_* / MASK_ORDER_BANDIT_3BIT
// constants are in CoreFrameworks/Order.hpp (see comment block there pointing here for the
// bit-width validity static_asserts). Order.hpp deliberately stays include-light; this
// header pulls ML-side symbols (FOREACH_BANDIT_ALGORITHM_COUNT / NUM_REGIMES /
// ENSEMBLE_HORIZON_MAX) and verifies each fits in 3 bits.
//
// If a future ship grows any of these past 8, this static_assert fires + bit allocation
// at Order.hpp needs reconfiguration (widen to 4-bit slots OR move bandit context out of
// flags_packed entirely).
//======================================================================================================
// 3-bit cap derived from MASK_ORDER_BANDIT_3BIT (Order.hpp) — value range is [0, MASK+1).
// If the mask widens (or shrinks), these caps follow automatically.
static_assert(::tt::MASK_ORDER_BANDIT_3BIT == 0x7u,
              "Order::flags_packed bandit slots assumed 3 bits (MASK=0x7); "
              "if widening the mask, update Order.hpp + the per-symbol caps below");

static_assert((unsigned)FOREACH_BANDIT_ALGORITHM_COUNT <= ((unsigned)::tt::MASK_ORDER_BANDIT_3BIT + 1u),
              "Order::flags_packed bandit_active_state slot is 3 bits (bits 17-19); "
              "FOREACH_BANDIT_ALGORITHM_COUNT must stay ≤ MASK_ORDER_BANDIT_3BIT+1 or widen the slot.");
static_assert((unsigned)NUM_REGIMES <= ((unsigned)::tt::MASK_ORDER_BANDIT_3BIT + 1u),
              "Order::flags_packed bandit_regime slot is 3 bits (bits 20-22); "
              "NUM_REGIMES must stay ≤ MASK_ORDER_BANDIT_3BIT+1 or widen the slot.");
static_assert((unsigned)ENSEMBLE_HORIZON_MAX <= ((unsigned)::tt::MASK_ORDER_BANDIT_3BIT + 1u),
              "Order::flags_packed bandit_chosen_arm slot is 3 bits (bits 23-25); "
              "ENSEMBLE_HORIZON_MAX must stay ≤ MASK_ORDER_BANDIT_3BIT+1 or widen the slot.");

//======================================================================================================
// [Pattern 1 REWARD DISPATCH TABLES — auto-derived from FOREACH_BANDIT_ALGORITHM metadata]
//======================================================================================================
// Per § A.1 of v5.15.5.F.4d merged plan body. Pattern 1 fn-pointer dispatch tables for REWARD
// ATTRIBUTION (separate from decision-time arm-selection in BanditAlgorithmRegistry.hpp).
//
// Structural target: adding a 6th bandit algorithm row to FOREACH_BANDIT_ALGORITHM with appropriate
// (exp3_up, thompson_up) metadata bits → BOTH buy + exit reward dispatch tables auto-extend. ZERO
// callsite changes at consumer reward-attribution sites. Closes Class 18 + Class 28 structurally
// for the reward-attribution dispatch family.
//
// PER-SIDE FIELD ACCESS:
//   - Buy:  ezoo->bandits[regime]              + ezoo->thompson_bandits[regime]
//   - Exit: ezoo->exit_bandits[regime]         + ezoo->thompson_exit_bandits[regime]
//
// Side selection via `BanditSide` enum tag + if-constexpr in leaf reward fns. Compile-time-resolved
// (zero runtime branch). Hand-mirror today; true symmetric rename + macro-name-concat auto-gen
// tracked at TECH_DEBT-084 for `.F.4f` cleanup ship.
//
// THOMPSON UPDATE THROUGH PATTERN 5 SINK:
//   thompson_only_reward + both_reward call `ezoo->thompson_update_fn(...)` (buy) or
//   `ezoo->exit_thompson_update_fn(...)` (exit) — Pattern 5 sink-fn-pointer dispatch. Per
//   ML_Headers/ThompsonBandit.hpp. Default = &noop_thompson_update; boot-wired to
//   &real_thompson_update at _InitThompsonBandits / _InitExitThompsonBandits when subsystem
//   actually initializes. Per-call cost: ~1-2ns indirect call (predicted to same target);
//   eliminates `if (thompson_active)` branch at every consumer.
//======================================================================================================

//------------------------------------------------------------------------------------------------------
// [SIDE TAG + DISPATCH CONTRACT TYPE]
//------------------------------------------------------------------------------------------------------
enum class BanditSide { Buy, Exit };

template <unsigned F>
using BanditRewardFn = void(*)(EnsembleModelZoo<F>* ezoo, int regime, int arm, double reward_bps);

//------------------------------------------------------------------------------------------------------
// [LEAF REWARD FNS — templated on Side via if-constexpr field selection]
//------------------------------------------------------------------------------------------------------
// Three leaf reward fns (one per algorithm-metadata combination). Each is templated on
// `BanditSide` for compile-time field selection — no runtime branch on side.
//
// `(exp3_up=1, thompson_up=0)` → exp3_only_reward  (Exp3 update only)
// `(exp3_up=0, thompson_up=1)` → thompson_only_reward  (Thompson update only via Pattern 5 sink)
// `(exp3_up=1, thompson_up=1)` → both_reward  (both updates; ghost-training modes + BLENDED)
// `(exp3_up=0, thompson_up=0)` → DEAD STATE (caught by FOREACH_BANDIT_ALGORITHM dead-state assert)

template <unsigned F, BanditSide Side>
inline void exp3_only_reward(EnsembleModelZoo<F>* ezoo, int regime, int arm, double r) {
    if constexpr (Side == BanditSide::Buy) {
        Bandit_Update(&ezoo->bandits[regime], arm, r);
    } else {
        Bandit_Update(&ezoo->exit_bandits[regime], arm, r);
    }
}

template <unsigned F, BanditSide Side>
inline void thompson_only_reward(EnsembleModelZoo<F>* ezoo, int regime, int arm, double r) {
    // Pattern 5 sink-fn dispatch — `*_thompson_update_fn` defaults to noop_thompson_update at
    // construction; boot-wires to real_thompson_update at _Init*ThompsonBandits when subsystem
    // enables. Consumer never branches on cfg/init state — uniform indirect call (H20 / Class 24
    // + Class 28 closure for reward dispatch family).
    if constexpr (Side == BanditSide::Buy) {
        ezoo->thompson_update_fn(&ezoo->thompson_bandits[regime], arm, r);
    } else {
        ezoo->exit_thompson_update_fn(&ezoo->thompson_exit_bandits[regime], arm, r);
    }
}

template <unsigned F, BanditSide Side>
inline void both_reward(EnsembleModelZoo<F>* ezoo, int regime, int arm, double r) {
    // Both update — used by EXP3_OP_THOMPSON_GHOST (cfg=2; Exp3 drives + Thompson shadow-learns)
    // + THOMPSON_OP_EXP3_GHOST (cfg=3; mirror) + BLENDED (cfg=4; both update; argmax over blend).
    // Per per-arm reward observability invariant (CoreModelZoo.hpp:881-882) — both bandits learn
    // from the same per-arm reward signal regardless of which one's CHOICE drove the trading decision.
    if constexpr (Side == BanditSide::Buy) {
        Bandit_Update(&ezoo->bandits[regime], arm, r);
        ezoo->thompson_update_fn(&ezoo->thompson_bandits[regime], arm, r);
    } else {
        Bandit_Update(&ezoo->exit_bandits[regime], arm, r);
        ezoo->exit_thompson_update_fn(&ezoo->thompson_exit_bandits[regime], arm, r);
    }
}

//------------------------------------------------------------------------------------------------------
// [BUY-SIDE REWARD DISPATCH TABLE — auto-derived from FOREACH_BANDIT_ALGORITHM (exp3_up, thompson_up)]
//------------------------------------------------------------------------------------------------------
// `?:` chain auto-selects one of 3 leaf fns per row. Adding a 6th algorithm row with metadata bits
// (e.g., `(1, 1)` → both_reward) extends this table by 1 entry — zero callsite changes.
//
// Consumer dispatch at reward-attribution sites (CoreModelZoo.hpp:1341/1402, ControllerEventLoop.hpp:1755):
//   int algo = core_cfg->bandit_algorithm;   // per-core read via Class 25 sweep (§ H of plan body)
//   tt::g_buy_reward_dispatch<F>[algo](ezoo, regime, arm, reward_bps);
// Branchless; ~1-2ns indirect call.

#define _BUY_REWARD_DISPATCH_ENTRY(name, val, fn, exp3_up, thompson_up, drives, doc) \
    [val] = ((exp3_up) && (thompson_up)) ? &both_reward<F, BanditSide::Buy>           \
          : (exp3_up)                    ? &exp3_only_reward<F, BanditSide::Buy>      \
          :                                 &thompson_only_reward<F, BanditSide::Buy>,
template <unsigned F>
constexpr BanditRewardFn<F> g_buy_reward_dispatch[FOREACH_BANDIT_ALGORITHM_COUNT] = {
    FOREACH_BANDIT_ALGORITHM(_BUY_REWARD_DISPATCH_ENTRY)
};
#undef _BUY_REWARD_DISPATCH_ENTRY

//------------------------------------------------------------------------------------------------------
// [EXIT-SIDE REWARD DISPATCH TABLE — same shape; FOREACH_BANDIT_SIDE auto-mirror]
//------------------------------------------------------------------------------------------------------
// Per FOREACH_BANDIT_SIDE meta-X-macro. Adding a 3rd side (e.g., per-symbol Thompson) would extend
// to 3 dispatch tables via copy-paste of this block with new `BanditSide::PerSymbol` tag — until
// TECH_DEBT-084 full symmetric rename enables single-template auto-gen.
//
// Consumer dispatch at exit-side reward-attribution sites (§ H of plan body):
//   tt::g_exit_reward_dispatch<F>[algo](ezoo, regime, arm, reward_bps);

#define _EXIT_REWARD_DISPATCH_ENTRY(name, val, fn, exp3_up, thompson_up, drives, doc) \
    [val] = ((exp3_up) && (thompson_up)) ? &both_reward<F, BanditSide::Exit>           \
          : (exp3_up)                    ? &exp3_only_reward<F, BanditSide::Exit>      \
          :                                 &thompson_only_reward<F, BanditSide::Exit>,
template <unsigned F>
constexpr BanditRewardFn<F> g_exit_reward_dispatch[FOREACH_BANDIT_ALGORITHM_COUNT] = {
    FOREACH_BANDIT_ALGORITHM(_EXIT_REWARD_DISPATCH_ENTRY)
};
#undef _EXIT_REWARD_DISPATCH_ENTRY

#endif // BANDIT_DISPATCH_TABLE_HPP
