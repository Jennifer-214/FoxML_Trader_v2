// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

#pragma once
//======================================================================================================
// [FILE]_[CoreFrameworks/NodeState.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the E.1.3 capital-plane skeletons (D-439 Phase-0) — NodeState (per-node ledger shell) + ClusterState (per-cluster shell) + AggregatorState (composer shell); VERSION-MANAGED in-memory growth per E.1.2 amendment 2: fields land phase-by-phase inside the pinned cluster lines, each add re-pins deliberately]
// [REFERENCE]_[DECISION]_[[D-439] [D-440]]
// [REFERENCE]_[INVARIANT]_[[H6] [H12] [H22]]
// [REFERENCE]_[DESIGN_SPEC]_[[decision-first-cluster-layout-pattern] [cross-thread-snapshot-publish-cluster-isolation]]
// [CONTAINS]
//   - [STRUCT]_[NodeState]
//   - [STRUCT]_[ClusterState]
//   - [STRUCT]_[AggregatorState]
//======================================================================================================
// WHY THIS HEADER EXISTS (Phase-0 of the merged E.1.3 ship, D-439/D-440)
//
// E.1.2 froze the WIRE (v11) but its NodeState/ClusterState skeleton never landed (0-hit symbols
// at every HEAD since; E.1.2 amendment 2 made in-memory growth leaf-by-leaf). These are the shells
// the merged coherence+fill ship relocates capital-plane state INTO:
//
//   NodeState<F>       — the per-node ledger row a node's OWN slow thread single-writes
//                        (H22: a node writes ONLY its own row; nothing here is written cross-thread).
//   ClusterState<F>    — the per-cluster shell (E.1.5 consumes; single-cluster deployment today —
//                        the cluster CAP lands with E.1.5's semantics, not here).
//   AggregatorState<F> — the composer's shell (sole writer: the central thread). Phase 1 grows it:
//                        FillEvent apply cursor + MoneySnapshot pack slot + kill word live HERE
//                        per the dive-v2 residence pin (cycle-free include topology).
//
// GROWTH DISCIPLINE (version-managed, per E.1.2 amendment 2): each phase REPLACES reserved words
// inside its named cluster line — or deliberately re-pins the size asserts below in the same
// commit. A silent size drift is a compile error, never a re-blessed golden.
//
// ⚠ NAME-COLLISION GUARDS (read before assuming):
//   - `CoreFrameworks/EventLoopAggregates.hpp` is the FLOAT_DISPLAY_ONLY TUI adapter — NOT this
//     aggregator. (Phase 2 rewires it into the MoneySnapshot→display shim.)
//   - `MemHeaders/NodeStateFlagRegistry.hpp` / `PerNodeStateFlagsRegistry.hpp` are the NodeContext
//     FLAG vocabulary ("NodeState SHALT" prose) — a different, pre-existing surface.
//   - `NodeSlowState` (ControllerEventLoop.hpp) is the per-node DATA plane (rolling/regime/flow);
//     NodeState here is the per-node CAPITAL plane. They are siblings, not duplicates.
//======================================================================================================
#include <cstdint>
#include <cstddef>
// NOTE: ../Limits.hpp joins at Phase 1 when the NodeArray<NodeState,...> carriers land — the
// Phase-0 shells reference no cap yet (unused-include hygiene).

//======================================================================================================
// [STRUCT]_[NodeState]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN] [CAPITAL_BEARING]]
// [THREAD]_[[NODE_SLOW_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-node capital-plane ledger row — four alignas(64) cluster lines, single-written by the owning node's slow thread (H22); Phase-0 shell: reserved words, offsets pinned]
//======================================================================================================
// [CODE]
//======================================================================================================
template <unsigned F>
struct alignas(64) NodeState {
    // ---- line 0 · cluster: slow_account (Phase 1/3 fill: node_realized/fees/peak/dd/unrealized
    //      rows the node single-writes and the composer reads via the published pack) ----
    alignas(64) uint64_t _slow_account_reserved[8] = {};   // H12: explicit zero-init

    // ---- line 1 · cluster: drainer_state (Phase 4 fill: absorbed per-node drain locals —
    //      bucket cursors, tail-drain state, per-node H8 histogram handle) ----
    alignas(64) uint64_t _drainer_state_reserved[8] = {};

    // ---- line 2 · cluster: kill_mirror (Phase 2 fill: the node's READ-ONLY mirror of the
    //      composer's kill word + per-node reset flag; the node never writes shared kill state) ----
    alignas(64) uint64_t _kill_mirror_reserved[8] = {};

    // ---- line 3 · cluster: binding (Phase 1 fill: typed-bridge/cfg binding — resolved
    //      PerNodeCfg ref, FillEvent ring handle, flatten flag) ----
    alignas(64) uint64_t _binding_reserved[8] = {};
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [END_STRUCT]_[NodeState]
//======================================================================================================

//======================================================================================================
// [STRUCT]_[ClusterState]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN] [CAPITAL_BEARING]]
// [THREAD]_[[COMPOSER_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-cluster shell — E.1.5 consumes (A34 governs on the slice); single-cluster deployment today, cluster CAP lands with E.1.5; Phase-0: one pinned line]
//======================================================================================================
// [CODE]
//======================================================================================================
template <unsigned F>
struct alignas(64) ClusterState {
    // ---- line 0 · cluster: slice (Phase 1 fill: the cluster's slice of the global aggregate +
    //      cluster kill-tier mirror; composer-written, E.1.5-read) ----
    alignas(64) uint64_t _slice_reserved[8] = {};   // H12: explicit zero-init
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [END_STRUCT]_[ClusterState]
//======================================================================================================

//======================================================================================================
// [STRUCT]_[AggregatorState]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [DATA_ORIENTED_DESIGN] [CAPITAL_BEARING]]
// [THREAD]_[[COMPOSER_WRITER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the composer's shell (sole writer = the central thread) — Phase 1 grows: FillEvent ring cursors + global ledger {balance, realized, ks_peak} + MoneySnapshot pack slot + the standalone atomic kill word; Phase-0: two pinned lines]
//======================================================================================================
// [CODE]
//======================================================================================================
template <unsigned F>
struct alignas(64) AggregatorState {
    // ---- line 0 · cluster: apply (Phase 1 fill: per-node FillEvent ring cursors + composer
    //      generation counter — the defined ring-order apply state) ----
    alignas(64) uint64_t _apply_reserved[8] = {};   // H12: explicit zero-init

    // ---- line 1 · cluster: ledger (Phase 1 fill: composer-owned global {balance, realized,
    //      ks_peak} — the per-fill-true ratchet lives here, D-441/source-purity pinned) ----
    alignas(64) uint64_t _ledger_reserved[8] = {};

    // NOTE (paper/live partition — D-441 ratification #4): modes are separate PROCESSES in this
    // engine, so mixed-mode totals cannot occur; if a mixed-mode deployment ever exists, the
    // partition hook belongs HERE (a second ledger line keyed by mode), not in the fill leaves.
};
//======================================================================================================
// [END_CODE]
//======================================================================================================
// [END_STRUCT]_[AggregatorState]
//======================================================================================================

// Phase-0 layout pins — growth is DELIBERATE (edit the assert in the same commit as the field),
// never drift. offsetof anchors per decision-first-cluster-layout-pattern.md Step 5.
static_assert(sizeof(NodeState<64>) == 256,      "NodeState<64> Phase-0 shell: 4 x 64B cluster lines");
static_assert(offsetof(NodeState<64>, _slow_account_reserved)  == 0,   "cluster line 0 anchor");
static_assert(offsetof(NodeState<64>, _drainer_state_reserved) == 64,  "cluster line 1 anchor");
static_assert(offsetof(NodeState<64>, _kill_mirror_reserved)   == 128, "cluster line 2 anchor");
static_assert(offsetof(NodeState<64>, _binding_reserved)       == 192, "cluster line 3 anchor");
static_assert(sizeof(ClusterState<64>) == 64,    "ClusterState<64> Phase-0 shell: 1 x 64B line");
static_assert(sizeof(AggregatorState<64>) == 128, "AggregatorState<64> Phase-0 shell: 2 x 64B lines");
static_assert(alignof(NodeState<64>) == 64 && alignof(ClusterState<64>) == 64 &&
              alignof(AggregatorState<64>) == 64, "capital-plane shells are cache-line aligned (H6)");
