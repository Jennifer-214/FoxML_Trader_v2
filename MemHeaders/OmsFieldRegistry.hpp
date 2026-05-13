// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ORDER MANAGER STATE — CANONICAL FIELD REGISTRY (v5.15.5.C.3)]
//======================================================================================================
// Single canonical X-macro registry covering OMS-level scalar fields with
// rich per-field attributes that drive THREE derived views:
//
//   FOREACH_OMS_INIT_FIELD     — boot-time value-init at OrderManager_Init
//   FOREACH_OMS_RESET_FIELD    — paper-reset value-init at the operator-
//                                 initiated session boundary
//   FOREACH_OMS_PERSIST_FIELD  — wire-format save/load via ShardedSnapshot
//                                 (Phase 6 archive flow reuses this)
//
// Replaces the prior `OmsPersistFieldRegistry.hpp` which had only the
// PERSIST view. Option A canonical (Caramel's choice 2026-05-13) closes
// the Class-18 mirror that prior approaches addressed via parallel
// registries + manual sync (the static_assert-count-match pattern):
// when 3 derived views overlap on the same field cohort (RESET ⊂ PERSIST
// for OMS), one canonical source eliminates drift at the source rather
// than catching it after the fact.
//
// Distinct from `.B.7`'s `FOREACH_CORE_CTX_*` precedent which used
// Option B (separate registries) because RESET and INIT diverged
// substantially (RESET was a curated subset of init for paper-session-
// reset semantics, with each membership decision anchored to a
// recurring-bug class). For OMS, RESET = PERSIST exactly (minus 3
// observability counters); Option A is structurally cheaper.
//
// Tuple shape: X(NAME, TYPE, INIT_VALUE, RESET_VALUE, RESET_KIND, PERSIST_KIND, PERSIST_MASK)
//
//   NAME          — field name on OrderManagerState (lowercase snake_case)
//   TYPE          — C++ field type; cast applied at write site via (TYPE)(value)
//   INIT_VALUE    — boot-time init value; may reference local variables in the
//                    AUTOPOPULATE scope (starting_balance, fee_rate, NOW_US, etc.)
//   RESET_VALUE   — paper-reset value; same shape as INIT_VALUE; often equal
//                    to INIT_VALUE but may differ (e.g. paper_session_start_us
//                    init = boot-time NOW_US; reset = reset-time NOW_US)
//   RESET_KIND    — DO_RESET | SKIP_RESET — token-paste dispatch:
//                    DO_RESET: field's reset_value is written at paper-reset
//                    SKIP_RESET: field is boot-only; paper-reset leaves it alone
//                                 (next_order_id, fee_rate*, adapter, etc.)
//   PERSIST_KIND  — DIRECT | BIT | SKIP_PERSIST — token-paste dispatch:
//                    DIRECT: read/write field directly as TYPE bytes
//                    BIT:    extract bit at save; set bit at load (kill_switch_tripped)
//                    SKIP_PERSIST: not in snapshot wire format (observability counters)
//   PERSIST_MASK  — for BIT-kind persist: the named bitmap mask constant
//                    (e.g. MASK_OMS_STATE_KILL_SWITCH_TRIPPED); for DIRECT
//                    and SKIP_PERSIST: 0
//
// Adding a new OMS-level field = ONE row in this registry. The 3 derived
// views auto-update. Cannot forget to update one view; cannot diverge
// across views; semantic intent (in_reset / in_persist / bit-extraction)
// is co-located with the field declaration for review-readability.
//
// Cross-references:
//   DESIGN_SPECS/registry-tuple-as-single-source-of-truth.md (THE pattern)
//   DESIGN_SPECS/heterogeneous-registry-pattern.md (multi-kind dispatch)
//   DESIGN_SPECS/autopopulate-pattern-for-production-caller-class.md
//   DESIGN_SPECS/x-macro-registry-with-presence-dispatch.md (SKIP_* dispatch)
//   DESIGN_SPECS/bitmap-flag-api.md (BIT-kind references mask constants)
//   DESIGN_SPECS/pre-post-cfg-registry-split-for-emit-order-preservation.md
//     (precedent for splitting one canonical registry into multiple views)
//   CLAUDE.md item 13 (X-macro registry)
//   CLAUDE.md item 15 (Parity-tested-by-construction — wire format)
//   CLAUDE.md item 19 (structural fix preferred for recurring class)
//   CLAUDE.md item 21 (AUTOPOPULATE companion macro)
//   v5.15.5.B.7 FOREACH_CORE_CTX_INIT_FIELD (Option B precedent; different fit)
//======================================================================================================
#ifndef OMS_FIELD_REGISTRY_HPP
#define OMS_FIELD_REGISTRY_HPP

#include <stdint.h>
#include <chrono>
#include "BitmapMacros.hpp"
#include "OmsStateFlagRegistry.hpp"  // MASK_OMS_STATE_* constants

namespace tt {

//======================================================================================================
// [HELPER: NOW_US — wall-clock microseconds since epoch]
//======================================================================================================
// Used as INIT_VALUE / RESET_VALUE for paper_session_start_us. Evaluated
// at the AUTOPOPULATE expansion site; each evaluation captures fresh time.
//======================================================================================================
inline uint64_t _oms_now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

//======================================================================================================
// [CANONICAL REGISTRY — OMS-LEVEL FIELDS]
//======================================================================================================
// Order matches the v5.15.5.B-aligned OrderManagerState struct layout
// (HOT/WARM/COLD/cross-thread clusters). Wire-format byte order for
// PERSIST view is preserved by listing fields in the same canonical
// order that the legacy save/load block used (see comment block at
// `ShardedSnapshotPersist.hpp:135-146` pre-v5.15.5.C.2 for the original
// hand-written order).
//
// SKIP_RESET reasoning per field group (audit trail for paper-reset
// EXCLUDED fields, anchored to historical incident class):
//
//   - next_order_id: drainer is running with in-flight orders; resetting
//                    risks ID collision with un-drained submits.
//   - adapter:       boot-set; resetting would zero function pointers,
//                    breaking live mode mid-session if operator paper-
//                    reset is ever called from live (defensive guard).
//   - oms_state_flags: contains LIVE_TRADING + PARTIAL_EXIT_ENABLED bits
//                      that are boot-set, not session-scoped. kill_switch
//                      bit reset handled separately by AUTOPOPULATE.
//   - fee_rate*, slippage_pct: cfg-derived; same cfg = same rates.
//   - last_seen_trade_id: high-watermark must monotonically increase
//                          across paper-reset (Class 5 recurring-bugs
//                          gap: replay-safety across the reset boundary).
//   - calibration_log_file, trade_log: opened FILE*/owner pointers.
//                                       Rotation happens at archive
//                                       capture (Phase 6) not at this
//                                       value-reset step.
//   - flatten_pending, recovery_until_us: cross-thread CAS atomics;
//                                          resetting via the value-init
//                                          path would race with CheckWsStaleness.
//                                          AUTOPOPULATE Layer 3 handles
//                                          atomic stores explicitly when
//                                          a reset of these is wanted
//                                          (not at paper-reset; this is
//                                          steady-state CAS state).
//   - event_log_mode, ks_min_balance, ks_max_drawdown_pct: cfg-derived.
//
// DO_RESET fields = session-scoped accumulator state (Phase 2.1 / Phase 3
// / v5.5.6 Class-5 recurring-bug class).
//
// Adding a new OMS-level field: append ONE row. Pick RESET_KIND and
// PERSIST_KIND by considering the field's session-scoped semantics +
// wire-format presence. All 3 derived views auto-update.
//======================================================================================================
#define FOREACH_OMS_FIELD(X)                                                                                          \
    /* ============================================================================================ */               \
    /* HOT cluster — drainer reads every cycle                                                       */               \
    /* ============================================================================================ */               \
    /* Order pool bookkeeping (scalars only; orders[] array + Order_Init loop is AUTOPOPULATE L2.  */               \
    /* event_log_mode handled in AUTOPOPULATE L2 — direct arg copy + sets up Layer 4 conditional.  */               \
    /* adapter handled in AUTOPOPULATE L2 — struct value copy from function param.                 */               \
    /* oms_state_flags handled in AUTOPOPULATE L2 — conditional LIVE_TRADING bit set from arg.     */               \
    X(order_bitmap,           uint16_t,  0,                            0,                            SKIP_RESET, SKIP_PERSIST, 0)  \
    X(next_order_id,          uint64_t,  1,                            1,                            SKIP_RESET, SKIP_PERSIST, 0)  \
    /* ============================================================================================ */               \
    /* COLD cluster — boot-set + paper-reset session state                                           */               \
    /* ============================================================================================ */               \
    /* Bank state — Phase 2.1 P&L leak anchor                                                       */               \
    X(balance,                FPN<F>,    starting_balance,             starting_balance,             DO_RESET,   DIRECT,       0)  \
    X(realized_pnl,           FPN<F>,    FPN_Zero<F>(),                FPN_Zero<F>(),                DO_RESET,   DIRECT,       0)  \
    /* Fee rates — cfg-derived; SKIP_RESET (same cfg = same rate post-reset)                         */               \
    X(fee_rate,               FPN<F>,    fee_rate,                     fee_rate,                     SKIP_RESET, SKIP_PERSIST, 0)  \
    X(fee_rate_maker,         FPN<F>,    fee_rate,                     fee_rate,                     SKIP_RESET, SKIP_PERSIST, 0)  \
    X(fee_rate_taker,         FPN<F>,    fee_rate,                     fee_rate,                     SKIP_RESET, SKIP_PERSIST, 0)  \
    X(slippage_pct,           FPN<F>,    FPN_Zero<F>(),                FPN_Zero<F>(),                SKIP_RESET, SKIP_PERSIST, 0)  \
    /* Maker/taker counters (v5.5.6 Class-5 recurring-bug close)                                    */               \
    X(maker_fills_count,      uint32_t,  0,                            0,                            DO_RESET,   DIRECT,       0)  \
    X(taker_fills_count,      uint32_t,  0,                            0,                            DO_RESET,   DIRECT,       0)  \
    X(total_maker_fees,       FPN<F>,    FPN_Zero<F>(),                FPN_Zero<F>(),                DO_RESET,   DIRECT,       0)  \
    X(total_taker_fees,       FPN<F>,    FPN_Zero<F>(),                FPN_Zero<F>(),                DO_RESET,   DIRECT,       0)  \
    X(total_fees,             FPN<F>,    FPN_Zero<F>(),                FPN_Zero<F>(),                DO_RESET,   DIRECT,       0)  \
    /* Exit-fill feedback channel masks (v5.13.0.B + v5.15.5.C.2)                                   */               \
    X(last_closed_mask,       uint16_t,  0,                            0,                            SKIP_RESET, SKIP_PERSIST, 0)  \
    X(last_opened_mask,       uint16_t,  0,                            0,                            SKIP_RESET, SKIP_PERSIST, 0)  \
    X(last_exit_predicted_bitmap, uint16_t, 0,                          0,                            SKIP_RESET, SKIP_PERSIST, 0)  \
    /* Kill-switch state (v5.15.5.C.2 S3a — kill_switch_tripped bit lives in oms_state_flags;        */               \
    /*                     ks_peak_balance + ks_trips_total below)                                  */               \
    X(ks_min_balance,         FPN<F>,    FPN_Zero<F>(),                FPN_Zero<F>(),                SKIP_RESET, SKIP_PERSIST, 0)  \
    X(ks_max_drawdown_pct,    FPN<F>,    FPN_Zero<F>(),                FPN_Zero<F>(),                SKIP_RESET, SKIP_PERSIST, 0)  \
    X(ks_peak_balance,        FPN<F>,    starting_balance,             starting_balance,             DO_RESET,   DIRECT,       0)  \
    X(ks_trips_total,         uint64_t,  0,                            0,                            SKIP_RESET, SKIP_PERSIST, 0)  \
    /* kill_switch_tripped — persisted as BIT-extracted from oms_state_flags (v5.15.5.C.2 S3a-W)    */               \
    X(kill_switch_tripped,    int,       0,                            0,                            DO_RESET,   BIT,          MASK_OMS_STATE_KILL_SWITCH_TRIPPED) \
    /* v5.15.5.C.3 (Phase 2) — paper-session start anchor for archive directory naming              */               \
    X(paper_session_start_us, uint64_t,  tt::_oms_now_us(),            tt::_oms_now_us(),            DO_RESET,   DIRECT,       0)  \
    /* Calibration log + trade log pointers (boot-set; rotation at archive capture)                  */               \
    X(calibration_log_file,   FILE*,     nullptr,                      nullptr,                      SKIP_RESET, SKIP_PERSIST, 0)  \
    X(trade_log,              ShardedTradeLog*, nullptr,               nullptr,                      SKIP_RESET, SKIP_PERSIST, 0)  \
    X(last_seen_trade_id,     uint64_t,  0,                            0,                            SKIP_RESET, SKIP_PERSIST, 0)

//======================================================================================================
// [PER-SLOT REGISTRY — FillRecord + adjacent per-slot arrays]
//======================================================================================================
// Separate from the canonical FOREACH_OMS_FIELD because per-slot fields
// have different access pattern (looped over MAX_PORTFOLIO_POSITIONS).
//
// Tuple: X(NAME_OR_ACCESSOR, TYPE, INIT_VALUE, RESET_VALUE)
//   NAME_OR_ACCESSOR — either a top-level array name (last_realized_return[i],
//                       last_exit_predicted_p[i]) or a FillRecord field
//                       accessor (last_fill[i].entry_notional, etc.)
//   TYPE             — C++ field type
//   INIT_VALUE       — per-slot init value
//   RESET_VALUE      — per-slot value at DrainPostFill post-attribution clear
//
// Note: last_fill[i].was_win uses int8_t = 0, but the field declaration
// in FillRecord is int8_t. Cast applied at write site.
//
// last_exit_predicted_meta[i] is cleared via OMS_META_CLEAR helper (single-byte
// zero with valid-bit 0), not raw `= 0`, for clarity. Handled separately in
// the per-slot AUTOPOPULATE expansion below.
//
// Walked at boot in OrderManager_Init AND at DrainPostFill per-slot
// post-attribution (the per-slot clear is now the OMS_RESET_PER_SLOT_
// EXIT_PREDICTOR macro shared between both sites — Class-18 mirror close
// per /merge-scan MEDIUM-1).
//======================================================================================================
#define FOREACH_OMS_PER_SLOT_FIELD(X)                                                          \
    X(last_realized_return[_i],         double,    0.0,            0.0)                        \
    X(last_fill[_i].entry_notional,     FPN<F>,    FPN_Zero<F>(),  FPN_Zero<F>())              \
    X(last_fill[_i].entry_fee,          FPN<F>,    FPN_Zero<F>(),  FPN_Zero<F>())              \
    X(last_fill[_i].exit_net_pnl,       FPN<F>,    FPN_Zero<F>(),  FPN_Zero<F>())              \
    X(last_fill[_i].exit_entry_notional,FPN<F>,    FPN_Zero<F>(),  FPN_Zero<F>())              \
    X(last_fill[_i].exit_total_fees,    FPN<F>,    FPN_Zero<F>(),  FPN_Zero<F>())              \
    X(last_fill[_i].was_win,            int8_t,    0,              0)                          \
    X(last_exit_predicted_p[_i],        double,    0.0,            0.0)

//======================================================================================================
// [COMPILE-TIME COUNT SENTINELS]
//======================================================================================================
#define _OMS_FIELD_COUNT_ONE(name, type, init, reset, rkind, pkind, pmask) +1
constexpr int FOREACH_OMS_FIELD_COUNT =
    0 FOREACH_OMS_FIELD(_OMS_FIELD_COUNT_ONE);
#undef _OMS_FIELD_COUNT_ONE

#define _OMS_PER_SLOT_COUNT_ONE(name, type, init, reset) +1
constexpr int FOREACH_OMS_PER_SLOT_FIELD_COUNT =
    0 FOREACH_OMS_PER_SLOT_FIELD(_OMS_PER_SLOT_COUNT_ONE);
#undef _OMS_PER_SLOT_COUNT_ONE

static_assert(FOREACH_OMS_FIELD_COUNT >= 22,
              "FOREACH_OMS_FIELD must keep at least the v5.15.5.C.3 set "
              "(22+ scalar entries). Removing entries requires explicit "
              "justification. AUTOPOPULATE L2-L4 fields (adapter, "
              "oms_state_flags, event_log_mode, per-slot, atomics) are "
              "NOT in this count.");

static_assert(FOREACH_OMS_PER_SLOT_FIELD_COUNT >= 8,
              "FOREACH_OMS_PER_SLOT_FIELD must keep the 8 per-slot scalar "
              "entries (last_realized_return + 6 FillRecord fields + last_exit_predicted_p).");

}  // namespace tt

//======================================================================================================
// [PROJECTION MACROS — derived view dispatch]
//======================================================================================================
// Each derived view is a token-paste dispatch on the appropriate KIND
// column. Per-kind macros generate the emission code (init assignment,
// reset assignment, save fwrite, load fread, load commit).
//======================================================================================================

// ---- INIT view — dispatch on PERSIST_KIND (BIT-kind fields don't exist as
//     direct fields; init writes to oms_state_flags bit instead) ----
// INIT is unconditional across all rkind values (every field gets init).
// PERSIST_KIND determines storage class for the init write:
//   DIRECT       — direct field write (_oms->name = ...)
//   SKIP_PERSIST — direct field write (storage exists; just not persisted)
//   BIT          — bit set/clear on oms_state_flags (no direct field)
#define OMS_PROJECT_INIT(name, type, init, reset, rkind, pkind, pmask) \
    OMS_PROJECT_INIT_##pkind(name, type, init, pmask)

#define OMS_PROJECT_INIT_DIRECT(name, type, init, pmask) \
    _oms->name = (type)(init);
#define OMS_PROJECT_INIT_SKIP_PERSIST(name, type, init, pmask) \
    _oms->name = (type)(init);
#define OMS_PROJECT_INIT_BIT(name, type, init, pmask) \
    do { \
        if ((int)(init)) _oms->oms_state_flags |= (uint8_t)(tt::pmask); \
        else             _oms->oms_state_flags &= (uint8_t)(~(uint8_t)(tt::pmask)); \
    } while (0);

// ---- RESET view — dispatch on RESET_KIND ----
#define OMS_PROJECT_RESET(name, type, init, reset, rkind, pkind, pmask) \
    OMS_PROJECT_RESET_##rkind(name, type, init, reset, pkind, pmask)

#define OMS_PROJECT_RESET_DO_RESET(name, type, init, reset, pkind, pmask) \
    OMS_PROJECT_RESET_DO_##pkind(name, type, init, reset, pmask)

#define OMS_PROJECT_RESET_SKIP_RESET(name, type, init, reset, pkind, pmask) \
    /* SKIP_RESET — no emission */

// DO_RESET dispatched on PERSIST_KIND for the BIT case (kill_switch_tripped
// resets via BITMAP_CLR on oms_state_flags, not a direct field write).
#define OMS_PROJECT_RESET_DO_DIRECT(name, type, init, reset, pmask) \
    _oms->name = (type)(reset);
#define OMS_PROJECT_RESET_DO_SKIP_PERSIST(name, type, init, reset, pmask) \
    _oms->name = (type)(reset);
#define OMS_PROJECT_RESET_DO_BIT(name, type, init, reset, pmask) \
    do { \
        if ((int)(reset)) _oms->oms_state_flags |= (uint8_t)(tt::pmask); \
        else              _oms->oms_state_flags &= (uint8_t)(~(uint8_t)(tt::pmask)); \
    } while (0);

// ---- PERSIST views (SAVE / LOAD-DECLARE / LOAD-READ / LOAD-COMMIT) ----
// Dispatch on PERSIST_KIND. SKIP_PERSIST emits nothing.

// SAVE: write field's wire value via fwrite.
#define OMS_PROJECT_PERSIST_SAVE(name, type, init, reset, rkind, pkind, pmask) \
    OMS_PROJECT_PERSIST_SAVE_##pkind(name, type, pmask)

#define OMS_PROJECT_PERSIST_SAVE_DIRECT(name, type, pmask)                                          \
    {                                                                                                \
        type _v_save_##name = state->oms->name;                                                      \
        if (fwrite(&_v_save_##name, sizeof(type), 1, f) != 1) goto fail;                             \
    }

#define OMS_PROJECT_PERSIST_SAVE_BIT(name, type, pmask)                                             \
    {                                                                                                \
        type _v_save_##name = (BITMAP_IS_SET(state->oms->oms_state_flags, tt::pmask) ? 1 : 0);       \
        if (fwrite(&_v_save_##name, sizeof(type), 1, f) != 1) goto fail;                             \
    }

#define OMS_PROJECT_PERSIST_SAVE_SKIP_PERSIST(name, type, pmask) \
    /* SKIP_PERSIST — no emission */

// LOAD: declare tmp_<name> variable per field.
#define OMS_PROJECT_PERSIST_DECLARE(name, type, init, reset, rkind, pkind, pmask) \
    OMS_PROJECT_PERSIST_DECLARE_##pkind(name, type)

#define OMS_PROJECT_PERSIST_DECLARE_DIRECT(name, type) type tmp_##name = {};
#define OMS_PROJECT_PERSIST_DECLARE_BIT(name, type)    type tmp_##name = {};
#define OMS_PROJECT_PERSIST_DECLARE_SKIP_PERSIST(name, type) /* no emission */

// LOAD: fread into tmp_<name>.
#define OMS_PROJECT_PERSIST_READ(name, type, init, reset, rkind, pkind, pmask) \
    OMS_PROJECT_PERSIST_READ_##pkind(name, type)

#define OMS_PROJECT_PERSIST_READ_DIRECT(name, type) \
    if (fread(&tmp_##name, sizeof(type), 1, f) != 1) { fclose(f); return 0; }
#define OMS_PROJECT_PERSIST_READ_BIT(name, type) \
    if (fread(&tmp_##name, sizeof(type), 1, f) != 1) { fclose(f); return 0; }
#define OMS_PROJECT_PERSIST_READ_SKIP_PERSIST(name, type) /* no emission */

// LOAD: commit tmp_<name> back to OMS.
#define OMS_PROJECT_PERSIST_COMMIT(name, type, init, reset, rkind, pkind, pmask) \
    OMS_PROJECT_PERSIST_COMMIT_##pkind(name, type, pmask)

#define OMS_PROJECT_PERSIST_COMMIT_DIRECT(name, type, pmask) \
    { state->oms->name = tmp_##name; }
#define OMS_PROJECT_PERSIST_COMMIT_BIT(name, type, pmask) \
    {                                                                                                \
        if (tmp_##name) state->oms->oms_state_flags |= (uint8_t)(tt::pmask);                         \
        else            state->oms->oms_state_flags &= (uint8_t)(~(uint8_t)(tt::pmask));             \
    }
#define OMS_PROJECT_PERSIST_COMMIT_SKIP_PERSIST(name, type, pmask) /* no emission */

//======================================================================================================
// [BACKWARD-COMPAT ALIASES — for consumers that import the old single-purpose registries]
//======================================================================================================
// These projection-driven aliases let existing consumers in
// `ShardedSnapshotPersist.hpp` (and the snapshot save/load code) keep
// their current call sites. New consumers can use FOREACH_OMS_FIELD
// directly + a custom X-projection.
//
// FOREACH_OMS_PERSIST_FIELD's old tuple was X(name, type, kind, mask) —
// 4-arg. We adapt by passing the OMS_PROJECT_PERSIST_* macros which
// expand from the canonical 7-arg tuple.
//======================================================================================================

// Old: FOREACH_OMS_PERSIST_FIELD(X) where X is X(name, type, kind, mask).
// New: FOREACH_OMS_FIELD(_X) where _X dispatches via PERSIST_KIND.
// For backward compat we provide aliases that consumers can call:
//
//   FOREACH_OMS_FIELD(OMS_PROJECT_PERSIST_SAVE)     — replaces OMS_PERSIST_DO_SAVE
//   FOREACH_OMS_FIELD(OMS_PROJECT_PERSIST_DECLARE)  — replaces OMS_PERSIST_DECLARE_TMP
//   FOREACH_OMS_FIELD(OMS_PROJECT_PERSIST_READ)     — replaces OMS_PERSIST_DO_READ
//   FOREACH_OMS_FIELD(OMS_PROJECT_PERSIST_COMMIT)   — replaces OMS_PERSIST_DO_COMMIT
//
// These are 1:1 replacements; SKIP_PERSIST fields emit nothing (correct
// behavior — observability counters aren't in the wire format).

// ---- PER-SLOT views ----
// Walked inside a for(_i=0..MAX_PORTFOLIO_POSITIONS) loop. Accessor includes
// [_i] so expansion produces `_oms->last_fill[_i].field = ...` etc.
#define OMS_PROJECT_PER_SLOT_INIT(accessor, type, init, reset) \
    _oms->accessor = (type)(init);
#define OMS_PROJECT_PER_SLOT_RESET(accessor, type, init, reset) \
    _oms->accessor = (type)(reset);

//======================================================================================================
// [AUTOPOPULATE COMPANION MACROS — multi-target dispatch]
//======================================================================================================
// OMS_INIT_AUTOPOPULATE(_oms_ptr, _adapter, _live_trading, _starting_balance,
//                        _fee_rate, _event_log_mode, _event_log_path)
//   Boot-time full init of an OrderManagerState. Covers:
//     Layer 1 — registry-driven value-init via FOREACH_OMS_FIELD walk
//     Layer 2 — special-case scalars (adapter struct copy, oms_state_flags
//                conditional LIVE_TRADING set, event_log_mode arg copy)
//     Layer 3 — alignas(64)-cluster atomic stores (flatten_pending,
//                recovery_until_us, total_submitted/filled/rejected)
//     Layer 4 — helper-Init (Portfolio_Init)
//     Layer 5 — per-slot init loops:
//                 - Order_Init across MAX_INFLIGHT_ORDERS
//                 - FOREACH_OMS_PER_SLOT_FIELD across MAX_PORTFOLIO_POSITIONS
//                 - OMS_META_CLEAR for last_exit_predicted_meta[]
//     Layer 6 — SPSC ring inits (result/ws_result/reconcile + 16 submit queues)
//     Layer 7 — conditional OrderEventLog init + LoadFromDisk + replay
//                 MUST RUN BEFORE Layer 8 (StartAsyncWriter depends on Init'd log)
//     Layer 8 — OrderEventLog_StartAsyncWriter (MUST RUN LAST)
//
// OMS_RESET_AUTOPOPULATE(_oms_ptr, _starting_balance)
//   Paper-reset of an OrderManagerState. Covers:
//     Layer 1 — registry-driven reset-subset value-init via FOREACH_OMS_FIELD
//                walk + OMS_PROJECT_RESET dispatch (SKIP_RESET fields emit no-op)
//     Layer 2 — atomic store reset for observability counters (Class 5 close)
//     Layer 3 — Portfolio_Init (clears all positions)
//
// Both macros expect the OMS pointer + supporting locals (starting_balance,
// fee_rate, etc.) as args; registry expansions reference these locally-
// shadowed variables.
//======================================================================================================

#define OMS_INIT_AUTOPOPULATE(_oms_ptr, _adapter, _live_trading, _starting_balance, _fee_rate, _event_log_mode, _event_log_path) \
    do {                                                                                              \
        auto* _oms = (_oms_ptr);                                                                       \
        FPN<F>      starting_balance = (_starting_balance);  /* macro-scoped for registry refs */     \
        FPN<F>      fee_rate         = (_fee_rate);          /* macro-scoped for registry refs */     \
        /* Layer 1 — registry value-init */                                                            \
        FOREACH_OMS_FIELD(OMS_PROJECT_INIT)                                                            \
        /* Layer 2 — special-case scalars */                                                           \
        _oms->adapter = (_adapter);                                                                    \
        _oms->oms_state_flags = 0;                                                                     \
        if (_live_trading) _oms->oms_state_flags |= (uint8_t)tt::MASK_OMS_STATE_LIVE_TRADING;          \
        _oms->event_log_mode = (_event_log_mode);                                                      \
        /* Layer 3 — atomic stores (alignas(64) clusters) */                                           \
        _oms->flatten_pending.store(0, std::memory_order_relaxed);                                     \
        _oms->recovery_until_us.store(0, std::memory_order_relaxed);                                   \
        _oms->total_submitted.store(0, std::memory_order_relaxed);                                     \
        _oms->total_filled.store(0, std::memory_order_relaxed);                                        \
        _oms->total_rejected.store(0, std::memory_order_relaxed);                                      \
        /* Layer 4 — helper-Init */                                                                    \
        Portfolio_Init(&_oms->portfolio);                                                              \
        /* Layer 5a — Order_Init across MAX_INFLIGHT_ORDERS (different scope from per-slot) */         \
        for (int _i = 0; _i < MAX_INFLIGHT_ORDERS; ++_i) {                                             \
            Order_Init(&_oms->orders[_i], 0, -1, ORDER_MARKET_BUY);                                    \
            _oms->orders[_i].state = ORDER_FILLED;                                                     \
        }                                                                                              \
        /* Layer 5b — FOREACH_OMS_PER_SLOT_FIELD + meta clear across MAX_PORTFOLIO_POSITIONS */        \
        for (int _i = 0; _i < MAX_PORTFOLIO_POSITIONS; ++_i) {                                         \
            FOREACH_OMS_PER_SLOT_FIELD(OMS_PROJECT_PER_SLOT_INIT)                                      \
            OMS_META_CLEAR(_oms->last_exit_predicted_meta[_i]);                                        \
        }                                                                                              \
        /* Layer 6 — SPSC ring inits */                                                                \
        SPSCRing_Init(&_oms->result_queue);                                                            \
        SPSCRing_Init(&_oms->ws_result_queue);                                                         \
        SPSCRing_Init(&_oms->reconcile_queue);                                                         \
        for (int _i = 0; _i < MAX_EXECUTION_CORES; ++_i) {                                             \
            SPSCRing_Init(&_oms->submit_queues[_i]);                                                   \
        }                                                                                              \
        /* Layer 7 — OrderEventLog conditional init + replay (MUST RUN BEFORE Layer 8) */              \
        {                                                                                              \
            const char* _evt_path = (_event_log_path);                                                  \
            int _has_disk_path = (_evt_path && _evt_path[0]);                                          \
            if ((_event_log_mode) == 1 && _has_disk_path) {                                            \
                OrderEventLog_Init(&_oms->event_log);                                                   \
                int _loaded = OrderEventLog_LoadFromDisk(&_oms->event_log, _evt_path);                 \
                if (_loaded > 0) {                                                                      \
                    FoldResult<F> _fold = Portfolio_FromEventLog(&_oms->event_log,                      \
                                                                  starting_balance, fee_rate);          \
                    _oms->portfolio    = _fold.portfolio;                                               \
                    _oms->balance      = _fold.balance;                                                 \
                    _oms->realized_pnl = _fold.realized_pnl;                                            \
                    if (FPN_GreaterThan(_oms->balance, _oms->ks_peak_balance))                          \
                        _oms->ks_peak_balance = _oms->balance;                                          \
                    std::fprintf(stderr, "[OMS] replayed %d events from disk, balance=$%.2f\n",         \
                                 _loaded, FPN_ToDouble(_oms->balance));                                 \
                }                                                                                       \
                OrderEventLog_InitWithFile(&_oms->event_log, _evt_path);                                \
            } else {                                                                                    \
                OrderEventLog_Init(&_oms->event_log);                                                   \
            }                                                                                          \
        }                                                                                              \
        /* Layer 8 — MUST RUN LAST */                                                                  \
        OrderEventLog_StartAsyncWriter(&_oms->event_log);                                              \
    } while (0)

#define OMS_RESET_AUTOPOPULATE(_oms_ptr, _starting_balance)                                            \
    do {                                                                                               \
        auto* _oms = (_oms_ptr);                                                                       \
        FPN<F> starting_balance = (_starting_balance);  /* macro-scoped for registry refs */          \
        /* Layer 1 — registry RESET walk; SKIP_RESET fields emit no-op */                              \
        FOREACH_OMS_FIELD(OMS_PROJECT_RESET)                                                           \
        /* Layer 2 — atomic stores for observability counters (Class 5 recurring-bug close) */         \
        _oms->total_submitted.store(0, std::memory_order_relaxed);                                     \
        _oms->total_filled.store(0, std::memory_order_relaxed);                                        \
        _oms->total_rejected.store(0, std::memory_order_relaxed);                                      \
        /* Layer 3 — Portfolio reset (clears all positions) */                                          \
        Portfolio_Init(&_oms->portfolio);                                                              \
    } while (0)

#endif  // OMS_FIELD_REGISTRY_HPP
