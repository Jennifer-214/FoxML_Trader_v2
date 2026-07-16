#pragma once
//======================================================================================================
// [FILE]_[CoreFrameworks/ShardedLiveSafety.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[live-safety helpers — orphan recovery (LIVE); force-close-on-shutdown (preserved but DISABLED + stale-signature, TECH_DEBT-192); reconcile lives in ReconciliationLoop]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-192]
// [CONTAINS]
//   - [FUNCTION]_[EngineSharded_OrphanRecovery]
//   - [FUNCTION]_[EngineSharded_ForceCloseOnShutdown]
//======================================================================================================
// SHARDED LIVE SAFETY — orphan recovery, force-close on shutdown,
// periodic reconciliation.
//
// Phase 0 of the sharded completion plan. Ports three live-trading safety
// features from legacy main.cpp (which only ran in single_core mode) into
// helpers callable from EngineSharded_Run. All three matter for any live
// use of sharded — without them, a crash + restart leaves orphan BTC,
// shutdowns can leave open positions, and manual Binance trades silently
// desync paper state from the exchange.
//
// CURRENT DISPOSITION (2026-06-13, TECH_DEBT-192 / D-211 sweep — supersedes the
// "callable from EngineSharded_Run" framing above for 0.2): only 0.1 orphan
// recovery is LIVE-called. 0.2 force-close was UNCALLED at v5.4.5 (positions
// persist across restart instead; the Run.hpp shutdown site documents why) and
// its body has since ROTTED (stale 9-positional OrderManager_Submit + pre-Ship-A
// FPN money types — compiles only because the template is uninstantiated). A
// re-enable is the .E.1 live-enable gate decision + requires the rewrite.
//
// All functions are paper-mode safe (early return when live_trading == 0
// or notify == nullptr).
//
// References to the legacy implementations they mirror:
//   0.1 orphan recovery        — main.cpp:290-340
//   0.2 force-close on shutdown — main.cpp:56-101
//   0.3 external trade reconcile — main.cpp:894-921
//======================================================================================================
#include "../DataStream/BinanceOrderAPI.hpp"
#include "BinanceAdapter.hpp"
#include "Notify.hpp"
#include "OrderManager.hpp"  // OrderManager_Submit for force-close
#include <cstdio>
#include <cstring>
#include <ctime>     // nanosleep

namespace tt {

//======================================================================
// [FUNCTION]_[EngineSharded_OrphanRecovery]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [BOOT_TIME] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[0.1 startup orphan recovery — non-zero venue BTC at boot is sold to USDT; balance-query failure refuses boot]
//======================================================================
// [CODE]
//======================================================================
// Query the exchange for current balances. If non-zero BTC exists at startup
// (sharded has no persistent state, so any BTC is by definition orphan from
// a prior session), market-sell it to recover USDT. Returns the post-
// recovery USDT balance via *usdt_out.
//
// On failure to query balances, returns false — caller should treat as
// fatal and refuse to start (better to halt than trade with unknown state).
//
// Notify alerts fire if notify != nullptr; otherwise alerts are stderr-only.
// Phase 4 (persistence) will refine this — once we have a snapshot of
// expected positions, we can reconcile rather than blindly selling.
static inline bool EngineSharded_OrphanRecovery(
    BinanceAdapterState* adapter,
    NotifyState* notify,
    double* usdt_out,
    double* btc_out)
{
    if (!adapter || adapter->worker_count < 1) {
        fprintf(stderr, "[sharded-safety] ORPHAN: adapter not initialized\n");
        return false;
    }
    BinanceOrderAPI* api = &adapter->workers_api[0];

    double usdt = 0.0, btc = 0.0;
    if (!BinanceOrderAPI_GetBalances(api, &usdt, &btc)) {
        fprintf(stderr, "[sharded-safety] ORPHAN: FATAL — could not query exchange balances\n");
        if (notify) {
            Notify_Send(notify, NOTIFY_CRITICAL, NK_ORPHAN_HALT,
                        "Engine halt — balance query failed",
                        "Could not query Binance balances at startup. "
                        "Refusing to start without exchange state.");
        }
        return false;
    }
    fprintf(stderr, "[sharded-safety] exchange balances at startup: USDT=$%.2f BTC=%.8f\n",
            usdt, btc);

    // Sharded has no startup snapshot (Phase 4 will add this), so any BTC
    // at boot is orphan by definition. Market-sell to recover USDT.
    // Threshold: 0.000001 BTC (~$0.07 at $70k) — below dust, can't be sold
    // anyway since lot_min_qty is typically 0.0001.
    if (btc > 0.000001) {
        double qty = binance_round_qty(btc, api->filters.lot_step_size);
        if (qty >= Money_ToDouble(api->filters.lot_min_qty)) {
            fprintf(stderr, "[sharded-safety] ORPHAN: %.8f BTC detected — selling to recover USDT\n",
                    qty);
            if (notify) {
                char body[256];
                snprintf(body, sizeof(body),
                         "Orphaned BTC %.8f detected at sharded startup. "
                         "Engine is auto-selling to recover USDT.",
                         qty);
                Notify_Send(notify, NOTIFY_WARN, NK_ORPHAN_DETECTED,
                            "Sharded orphan recovery at startup", body);
            }
            char order_id[32] = {};
            double fill_price = 0.0, fill_qty = 0.0;
            int sell_ok = BinanceOrderAPI_MarketSell(api, qty, order_id, &fill_price, &fill_qty);
            if (!sell_ok) {
                fprintf(stderr, "[sharded-safety] ORPHAN: market sell FAILED — leaving orphan in place\n");
                if (notify) {
                    Notify_Send(notify, NOTIFY_ALERT, NK_ORPHAN_HALT,
                                "Sharded orphan sell failed",
                                "Market sell to recover orphan BTC failed. "
                                "Manual intervention required.");
                }
                // Don't fail boot — let user decide. Engine starts with btc still on
                // exchange, paper state will know about it via reconciliation later.
            } else {
                fprintf(stderr, "[sharded-safety] ORPHAN: sold %.8f @ $%.2f\n",
                        fill_qty, fill_price);
                // Re-query balances post-sell to get accurate starting USDT
                if (!BinanceOrderAPI_GetBalances(api, &usdt, &btc)) {
                    fprintf(stderr, "[sharded-safety] ORPHAN: post-sell balance query failed\n");
                    return false;
                }
                fprintf(stderr, "[sharded-safety] post-recovery balances: USDT=$%.2f BTC=%.8f\n",
                        usdt, btc);
            }
        } else {
            fprintf(stderr, "[sharded-safety] ORPHAN: %.8f BTC below lot_min_qty %.8f — leaving as dust\n",
                    btc, Money_ToDouble(api->filters.lot_min_qty));
        }
    }

    if (usdt_out) *usdt_out = usdt;
    if (btc_out)  *btc_out  = btc;
    return true;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineSharded_OrphanRecovery]
//======================================================================

//======================================================================
// [FUNCTION]_[EngineSharded_ForceCloseOnShutdown]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [SUPPORTIVE]]
// [REFERENCE]_[TECH_DEBT]_[TECH_DEBT-192]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[0.2 force-close on shutdown — UNCALLED since v5.4.5 (persist-across-restart won) + body ROTTED vs the F.4c.3 Submit signature; rewrite gated on the .E.1 live-enable decision, TECH_DEBT-192]
//======================================================================
// [CODE]
//======================================================================
// Walk the OMS portfolio bitmap, submit a market sell for each open position
// via the BinanceAdapter queue, then wait (with timeout) for the bitmap to
// clear as fills arrive through the user-data WS → drainer → OnEvent path.
// Called on quit signal BEFORE the executor / drainer threads are joined,
// so the normal exit flow can process the resulting fills.
//
// Refuses to silently exit with open positions — the legacy guarantee from
// main.cpp:56-101 was "we NEVER reconnect with orphaned positions." Same
// applies here: graceful shutdown means a flat book.
//
// Returns the number of positions still open after the timeout (0 = clean).
// Caller logs/alerts based on the return value.
//
// Architecture note: uses queue-based BinanceAdapter_SubmitMarketSell, NOT
// direct BinanceOrderAPI_MarketSell on workers_api[0]. The worker thread
// owns workers_api[0] and we'd race if we touched it directly.
template <unsigned F>
static inline int EngineSharded_ForceCloseOnShutdown(
    OrderManagerState<F>* oms,
    BinanceAdapterState* adapter,
    NotifyState* notify,
    const PerNodeCfg<F>* nodes,
    int timeout_secs = 30)
{
    // v5.15.5.F.4c.3 WIP2d-1.B.1: `cores` REQUIRED — per-core array pointer (caller passes
    // `cfg.nodes`). Multi-slot dispatch fn; per cfg-scope-discipline § "consumer over per-core
    // array." First canonical of the "consumer over per-core array" sig shape.
    if (!oms) return 0;
    uint16_t bitmap = oms->portfolio.active_bitmap;
    if (bitmap == 0) return 0;  // nothing to do — clean shutdown

    int initial_count = __builtin_popcount(bitmap);
    fprintf(stderr, "[sharded-safety] FORCE-CLOSE: %d open position(s) at shutdown — closing before exit\n",
            initial_count);
    if (notify) {
        char body[256];
        snprintf(body, sizeof(body),
                 "Engine shutdown with %d open position(s). "
                 "Force-closing via market sell before exit.",
                 initial_count);
        Notify_Send(notify, NOTIFY_WARN, NK_ORPHAN_DETECTED,
                    "Sharded shutdown — force closing", body);
    }

    // v5.15.5.C.2 (S3a) — bit-packed in oms_state_flags.
    if (!BITMAP_IS_SET(oms->oms_state_flags, tt::MASK_OMS_STATE_LIVE_TRADING)) {
        // Paper mode: clear the portfolio locally. No exchange state to manage.
        oms->portfolio.active_bitmap = 0;
        fprintf(stderr, "[sharded-safety] FORCE-CLOSE: paper mode — cleared %d position(s) locally\n",
                initial_count);
        return 0;
    }

    if (!adapter || adapter->worker_count < 1) {
        fprintf(stderr, "[sharded-safety] FORCE-CLOSE: no live adapter available — cannot close\n");
        return initial_count;
    }

    // Submit a market sell for each open slot via OrderManager_Submit. This
    // is the same code path that regular SL/TP exits use, so fills come back
    // through the user-data WS → OMS result queue → drainer → OnEvent path
    // and clear the bitmap bit naturally. node_id == slot under sharded's
    // single-position-per-core invariant.
    //
    // v5.15.5.F.4c.3 WIP2d-1.B.1 — bitmap iteration via __builtin_ctz (H20 + branchless-dispatch-discipline.md
    // Pattern 3 sub-variant). Replaces prior `for (slot=0..16) if (!(bitmap & ...)) continue;` per-slot
    // data-dependent gate with branchless bit scan: one tzcnt + one clear-lowest-bit per active slot. Inactive
    // slots cost ZERO cycles (not iterated). Same pattern FlattenAll uses; canonical bitmap iteration.
    BinanceOrderAPI* api = &adapter->workers_api[0];  // for filter access only
    uint16_t bm = bitmap;
    while (bm) {
        int slot = __builtin_ctz(bm);
        bm &= (uint16_t)(bm - 1);
        FPN_Binary<F> qty = oms->portfolio.positions[slot].quantity;
        double qty_d = FPN_ToDouble(qty);
        qty_d = binance_round_qty(qty_d, api->filters.lot_step_size);
        if (qty_d < Money_ToDouble(api->filters.lot_min_qty)) {
            fprintf(stderr, "[sharded-safety] FORCE-CLOSE slot %d: qty %.8f below min — skipping\n",
                    slot, qty_d);
            continue;
        }
        FPN_Binary<F> qty_rounded = FPN_FromDouble<F>(qty_d);
        // v5.15.5.F.4c.3 WIP2d-1.B.1 — per-core cfg for Order_BindPreResolved at submit.
        // Under sharded single-position-per-core invariant, slot == node_id; nodes[slot] is the
        // originating core's cfg.
        uint64_t order_id = OrderManager_Submit<F>(oms, (int16_t)slot, ORDER_MARKET_SELL,
                                                    qty_rounded,
                                                    FPN_Zero<F>(), FPN_Zero<F>(),
                                                    /*strategy_id*/(uint8_t)0xFF,
                                                    FPN_Zero<F>(),
                                                    /*leg*/(uint8_t)0,
                                                    &nodes[slot]);
        if (order_id == 0) {
            fprintf(stderr, "[sharded-safety] FORCE-CLOSE slot %d: OMS rejected submission\n", slot);
        } else {
            fprintf(stderr, "[sharded-safety] FORCE-CLOSE slot %d: queued sell of %.8f (order id %llu)\n",
                    slot, qty_d, (unsigned long long)order_id);
        }
    }

    // Wait up to timeout_secs for the bitmap to drain. Drainer thread is
    // still running at this point — it processes fills as they arrive.
    int waited_ms = 0;
    const int poll_ms = 100;
    while (oms->portfolio.active_bitmap != 0 && waited_ms < timeout_secs * 1000) {
        struct timespec ts{0, poll_ms * 1000000L};
        nanosleep(&ts, nullptr);
        waited_ms += poll_ms;
    }

    int remaining = __builtin_popcount(oms->portfolio.active_bitmap);
    if (remaining == 0) {
        fprintf(stderr, "[sharded-safety] FORCE-CLOSE: all positions flattened (%d ms)\n", waited_ms);
    } else {
        fprintf(stderr, "[sharded-safety] FORCE-CLOSE: TIMEOUT — %d position(s) still open after %ds\n",
                remaining, timeout_secs);
        if (notify) {
            char body[256];
            snprintf(body, sizeof(body),
                     "Force-close TIMEOUT for %d position(s) after %ds. "
                     "Manual intervention required to flatten on Binance.",
                     remaining, timeout_secs);
            Notify_Send(notify, NOTIFY_ALERT, NK_ORPHAN_HALT,
                        "Sharded force-close incomplete", body);
        }
    }
    return remaining;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[EngineSharded_ForceCloseOnShutdown]
//======================================================================

//----------------------------------------------------------------------
// [SECTION]_[0.3 — periodic reconciliation (already implemented — lives in ReconciliationLoop.hpp)]
//----------------------------------------------------------------------
// Sharded already has this via ReconciliationLoop (see ReconciliationLoop.hpp,
// started by EngineSharded_Run). The existing implementation is more
// sophisticated than a naive balance comparison — it accounts for inflight
// buy orders that have been submitted but not yet filled (the exchange has
// reserved the funds, the OMS hasn't booked the position yet).
//
// On drift, ReconciliationLoop_Pass pushes CMD_RECONCILE to the OMS reconcile
// queue, which the drainer processes for correction.
//
// Future enhancement: ReconciliationLoop logs to stderr on drift but doesn't
// call Notify_Send. Adding notify integration there would surface drift to
// Discord/Slack. Tracked separately.
//======================================================================================================

}  // namespace tt
