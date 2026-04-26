// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [EXCHANGE ADAPTER]
//
// Generic exchange interface used by the OMS. Concrete adapters wrap a
// specific exchange API behind this interface so the OMS can stay
// exchange-agnostic. Phase 02 ships BinanceAdapter; phase 08 adds a
// MockAdapter for tests; future phases can add Coinbase, Kraken, etc.
//
// The interface is a struct of function pointers + an opaque ctx pointer.
// This is the C-style polymorphism the codebase already uses everywhere
// (no virtuals, no classes — see CLAUDE.md "Code Conventions").
//
// Async submission contract:
//   submit_market_buy / submit_market_sell return IMMEDIATELY after
//   enqueueing the request to the adapter's worker thread. They do NOT
//   block on the network. The supplied callback fires (from the worker
//   thread) once the order completes — success or failure. Callers must
//   make the callback thread-safe; the OMS does this by pushing a
//   CMD_FILL_RESULT into its single-consumer result queue.
//
// Sync calls (get_balances, query_order):
//   These are slow-path queries called from controller-side code, NOT
//   from any hot path. They block on the network. Used by reconciliation
//   (phase 05) and startup balance sync, not on every tick.
//
// OrderResult is non-templated POD so it can flow through the OMS
// command queue without depending on the FPN<F> width.
//======================================================================================================

#pragma once

#include <cstdint>

namespace tt {

// Result of a single order submission. Populated by the adapter worker
// thread and passed to the OrderCallback. POD so it can be memcpy'd into
// SPSCRing slots without ceremony.
struct OrderResult {
    int      success;             // 1 = filled (or partial), 0 = failed
    char     exchange_id[64];     // assigned by exchange on success, "" on failure
    double   avg_fill_price;      // weighted average across partials
    double   fill_qty;            // total filled quantity
    int      error_code;          // exchange-specific error code, 0 on success
    char     error_message[128];  // human-readable error string
    // Phase 8 — Binance fill type for maker/taker fee accounting.
    // Set by ud_parse_execution_report from the "m" field on WS fills.
    // Synchronous REST fills (Phase 02) leave this 0 (assume taker — Binance
    // market orders are always taker by definition). Backtest path uses 0.
    uint8_t  is_maker;            // 1 = maker fill, 0 = taker (default)
    uint8_t  order_complete;      // 1 = "X":"FILLED", 0 = "X":"PARTIALLY_FILLED"
    double   commission;          // Binance "n" — commission paid this fill (in commission_asset units)
    char     commission_asset[8]; // "BNB", "USDT", "BTC" — Binance "N" field
};

// Callback signature: the adapter calls this when an order completes.
// user_ctx is the opaque pointer the OMS passed when submitting (typically
// the OrderManagerState<F>* cast to void*). client_id is the local order
// id assigned by the OMS at submission time. result points to a transient
// OrderResult — the callback must copy out anything it needs before returning.
typedef void (*OrderCallback)(void* user_ctx,
                               uint64_t client_id,
                               const OrderResult* result);

// Generic adapter interface. Each concrete adapter (BinanceAdapter,
// MockAdapter, etc.) populates a value of this struct via its _Get function
// and the OMS embeds it by value inside OrderManagerState. The ctx pointer
// stays valid for the lifetime of the adapter (typically the lifetime of
// the engine) so the OMS can pass it back to every method.
//
// All function pointers must be non-null when the adapter is wired into
// the OMS. There is no "optional method" — adapters that don't support
// query_order should provide a stub that returns 0 with an error code.
//
// The template parameter F lets the adapter type-check against FPN<F>
// fields if it needs them. Phase 02 BinanceAdapter doesn't actually use F
// in its function signatures (it converts at the boundary) but keeping
// the template makes future adapters easier.
template <unsigned F>
struct ExchangeAdapter {
    // Async submit. Returns 1 on enqueue success, 0 on enqueue failure
    // (queue full). The callback fires later from the adapter's worker
    // thread when the underlying exchange call completes.
    int (*submit_market_buy)(void* ctx, uint64_t client_id, double qty,
                              OrderCallback cb, void* user);
    int (*submit_market_sell)(void* ctx, uint64_t client_id, double qty,
                               OrderCallback cb, void* user);

    // Synchronous queries (slow path only, not hot path). Return 1 on
    // success, 0 on failure. Implementations may block on the network.
    int (*get_balances)(void* ctx, double* base_out, double* quote_out);
    int (*query_order)(void* ctx, const char* exchange_id, OrderResult* out);

    // Lifecycle.
    void (*shutdown)(void* ctx);

    // Opaque adapter state. Set once at construction, read by every method.
    void* ctx;
};

}  // namespace tt
