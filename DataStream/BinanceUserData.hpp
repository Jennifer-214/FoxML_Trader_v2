// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FILE]_[DataStream/BinanceUserData.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CAPITAL_BEARING] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the executionReport fill stream — WS + keepalive thread pair push CMD_WS_FILL into the OMS SPSC ring; KNOWN OPEN CAPITAL FINDINGS on the parser (TECH_DEBT-169/171, see the parse block)]
// [CONTAINS]
//   - [STRUCT]_[BinanceUserDataState]
//   - [FUNCTION]_[ud_ws_close]   (the ud_* state wrapper over the shared ws_* frame family)
//   - [FUNCTION]_[ud_consume_frame]   (the per-frame body, templated over the Io seam — suite-drivable)
//   - [FUNCTION]_[ud_obtain_listen_key]   (+ ud_keepalive_listen_key)
//   - [FUNCTION]_[ud_parse_execution_report]
//   - [FUNCTION]_[ud_ws_thread]
//   - [FUNCTION]_[ud_keepalive_thread]
//   - [FUNCTION]_[BinanceUserData_Init]   (+ Start / Shutdown family)
//======================================================================================================
//
// Websocket subscriber for Binance's user data stream. Receives real-time
// executionReport events (fills, rejections, cancellations) and pushes them
// into the OMS via a dedicated SPSC ring.
//
// Phase 04 of the OMS plan — see plans/oms/04_user_data_websocket/plan.md.
//
// Architecture:
//   Two threads spawned by BinanceUserData_Start:
//
//   1. WS thread — obtains a listen key via POST /api/v3/userDataStream,
//      connects WSS to /ws/<listenKey>, reads frames in a loop, parses
//      executionReport JSON, pushes CMD_WS_FILL into ws_result_queue.
//      On disconnect: sleep, re-obtain key, reconnect.
//
//   2. Keepalive thread — every 25 minutes, PUTs /api/v3/userDataStream
//      with the current listen key to refresh it (Binance expires at 60 min).
//      On failure: sets a flag that forces the WS thread to reconnect.
//
// REST calls use a dedicated BinanceOrderAPI instance (not shared with the
// adapter workers). Follows the established rule: per-thread instances,
// never share, never lock. The listen key REST calls need the X-MBX-APIKEY
// header but NOT HMAC signing (Binance docs: userDataStream endpoints are
// API-key-only, not signed).
//
// The ws_result_queue pointer is stored at init time. It points into the
// OMS's OrderManagerState. The WS thread is the sole producer; the drainer
// thread is the sole consumer via OrderManager_Tick. SPSC contract holds.
//
// Reuses the same SSL/WebSocket patterns from DataStream/BinanceCrypto.hpp
// (tcp_connect, tls_setup, ws_handshake, ws_read_frame, ws_send_pong).
// The functions are duplicated here as static inlines with a "ud_" prefix
// because BinanceCrypto.hpp's versions operate on BinanceStream* and have
// market-data-specific fields mixed in. The WS protocol code is ~120 lines
// and proven stable across months of live market data streaming.
//======================================================================================================

#pragma once

#include "../CoreFrameworks/ExchangeAdapter.hpp"
#include "../CoreFrameworks/SPSCRing.hpp"
#include "../CoreFrameworks/OrderManager.hpp"
#include "../CoreFrameworks/Notify.hpp"  // Phase 8b — disconnect alerts
#include "BinanceOrderAPI.hpp"
#include "WebSocketUtil.hpp"          // the ONE RFC 6455 frame family (PARITY-071 / D-487)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>

#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>

namespace tt {

//======================================================================
// [STRUCT]_[BinanceUserDataState]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CONCURRENCY]]
// [THREAD]_[[UD_WS_WRITER] [ENGINE_READER]]
// [STRADDLE_EXEMPT]_[ws_host]_[init-only host string — written once at boot, read-shared thereafter (no steady-state writes ⇒ no ping-pong) — D-414 leaf-3 2026-08-10]
// [STRADDLE_EXEMPT]_[rest_host]_[init-only host string — written once at boot, read-shared thereafter (no steady-state writes ⇒ no ping-pong) — D-414 leaf-3 2026-08-10]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[WS-thread-owned connection state + dedicated REST instance (listen key) + the OMS SPSC ring pointer + shutdown/keepalive flags + atomic TUI observability counters]
//======================================================================
// [CODE]
//======================================================================
struct BinanceUserDataState {
    // Connection state (WS thread owned)
    int      sockfd;
    SSL_CTX* ssl_ctx;
    SSL*     ssl;
    char     listen_key[128];
    int      connected;

    // Credentials (for listen key REST calls — own instance, not shared)
    BinanceOrderAPI rest_api;
    char     ws_host[64];          // "stream.binance.vision" or testnet
    char     rest_host[64];        // "testnet.binance.vision" or "api.binance.us"

    // Output: SPSC ring for fill events → OMS drainer
    OmsCmdRings* ws_rings;          // the OMS's per-node WS fill rings (3b(ii) commit 4)
    // D-479: the GLOBAL kill-trip request word (&agg.kill_trip_request). Stored as a pointer at
    // init because this file is F-independent and cannot name the aggregator's template type.
    std::atomic<uint32_t>* kill_trip_word;
    std::atomic<uint64_t> foreign_fills;    // executionReports for orders that are not ours
    std::atomic<uint64_t> ws_push_retries;  // pushes that needed the bounded wait to clear
    std::atomic<uint64_t> ws_push_fatal;    // bounded pushes that gave up -> LOUD-FATAL

    // Threads
    std::thread ws_thread;
    std::thread keepalive_thread;
    std::atomic<int> shutdown_requested;
    std::atomic<int> keepalive_failed;  // WS thread checks this to force reconnect

    // Observability (atomic for TUI reads from render thread)
    std::atomic<int>      ws_connected;     // 1 when WSS is live
    std::atomic<uint64_t> fills_received;
    std::atomic<uint64_t> events_received;  // all events (fills + non-fills)
    std::atomic<uint64_t> reconnect_count;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [ORIGIN]_[AUTO]
// [UPDATED]_[2026-09-06]
// [SIZE]_[880B]
// [ALIGN]_[16]
// [CACHE_LINES]_[14]
// [STRADDLE]_[ws_host@656 · rest_host@720]
//======================================================================
// [END_STRUCT]_[BinanceUserDataState]
//======================================================================

//======================================================================
// [FUNCTION]_[ud_ws_close]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ud_* WS state wrapper over the SHARED frame family — closes the socket through ws_close and clears the state the shared body knows nothing about (connected + the ws_connected atomic the REST adapter's ACK arm reads); the private tcp_connect / tls_setup / handshake / reader / pong copies were DELETED at the 2026-09-05 leaf's commit 4, closing PARITY-071]
//----------------------------------------------------------------------
// tcp_connect / tls_setup / handshake / frame reader / pong USED to live here as a PRIVATE copy,
// the THIRD hand-written RFC 6455 client in this directory. Its own header said so — "duplicated
// from BinanceCrypto with the ud_ prefix". That duplication is PARITY-071, and it cost a
// session-killing production stall when the depth copy drifted (D-487): three bodies, nothing
// comparing them, each locally plausible. This is the last one; the family is now single-bodied.
//
// The two holes this copy carried, both closed by consuming the shared body:
//
//   - `if ((int)pay_len > buf_size-1) return -1;` narrowed the 64-bit length BEFORE the bounds
//     check and then wrote `buf[pay_len] = '\0'` with the WIDE value. A 127-form length of
//     2^32+5 casts to 5, passes the guard, consumes 5 bytes of a 4-billion-byte frame, and
//     NUL-writes ~4 GB past a 4096-byte stack buffer. `ws_read_frame` compares as uint64_t
//     before any narrowing. This is the CAPITAL path — the frame it mis-reads carries fills.
//   - `ud_ws_send_pong` built into a 256-byte stack frame with NO capacity check while the
//     reader handed it payloads from a 4096-byte buffer. `ws_build_pong` refuses what will not
//     fit, and the reader now rejects an oversize control frame (RFC 6455 §5.5) first.
//
// `ud_ws_close` SURVIVES as a thin wrapper: it owns state the shared `ws_close` knows nothing
// about (`connected`, the `ws_connected` atomic the REST adapter's ACK arm reads).
//======================================================================
// [CODE]
//======================================================================
static inline void ud_ws_close(BinanceUserDataState* s) {
    if (s->ssl) {
        ws_close(s->ssl, s->ssl_ctx, s->sockfd);      // masked close frame -> shutdown -> free -> close(fd)
    } else {
        // ws_ssl_setup writes its out-params only on success, so a NULL ssl implies a NULL ctx;
        // the ctx branch is kept because this is also reached from paths that never called it.
        if (s->ssl_ctx) SSL_CTX_free(s->ssl_ctx);
        if (s->sockfd >= 0) close(s->sockfd);
    }
    s->ssl = NULL; s->ssl_ctx = NULL; s->sockfd = -1;
    s->connected = 0;
    s->ws_connected.store(0, std::memory_order_relaxed);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ud_ws_close]
//======================================================================

//======================================================================
// [FUNCTION]_[ud_obtain_listen_key]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[listen-key REST pair (ud_keepalive_listen_key rides) — POST with exponential backoff (1..30s, shutdown-interruptible) / PUT refresh; API-key-only, no HMAC]
//======================================================================
// [CODE]
//======================================================================
static inline int ud_obtain_listen_key(BinanceUserDataState* s) {
    int delays[] = {0, 1, 2, 4, 8, 15, 30};
    int max_attempts = 7;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (attempt > 0) {
            fprintf(stderr, "[UserData] listen key retry %d/%d after %ds\n",
                    attempt, max_attempts-1, delays[attempt]);
            for (int i = 0; i < delays[attempt]*10; ++i) {
                if (s->shutdown_requested.load(std::memory_order_acquire)) return 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        char body[512];
        int status = binance_rest_request(&s->rest_api, "POST",
                                           "/api/v3/userDataStream", "",
                                           body, sizeof(body));
        if (status == 200) {
            char key[128] = {};
            binance_json_extract_str(body, "listenKey", key, sizeof(key));
            if (key[0] != '\0') {
                strncpy(s->listen_key, key, sizeof(s->listen_key)-1);
                s->listen_key[sizeof(s->listen_key)-1] = '\0';
                fprintf(stderr, "[UserData] obtained listen key: %.20s...\n", s->listen_key);
                return 1;
            }
            fprintf(stderr, "[UserData] listen key empty in response: %s\n", body);
        } else {
            fprintf(stderr, "[UserData] listen key POST failed (status %d): %s\n", status, body);
        }
    }
    fprintf(stderr, "[UserData] CRITICAL: all listen key attempts exhausted\n");
    return 0;
}

// PUT /api/v3/userDataStream?listenKey=<key> — refresh keepalive.
static inline int ud_keepalive_listen_key(BinanceUserDataState* s) {
    char params[256];
    snprintf(params, sizeof(params), "listenKey=%s", s->listen_key);
    char body[512];
    int status = binance_rest_request(&s->rest_api, "PUT",
                                       "/api/v3/userDataStream", params,
                                       body, sizeof(body));
    if (status != 200) {
        fprintf(stderr, "[UserData] keepalive PUT failed (status %d): %s\n", status, body);
        return 0;
    }
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// POST /api/v3/userDataStream — creates a listen key. API-key-only, no HMAC.
// Retries with exponential backoff (1s, 2s, 4s, 8s, max 30s). Returns 1 on
// success (listen_key populated), 0 after all retries exhausted.
//======================================================================
// [END_FUNCTION]_[ud_obtain_listen_key]
//======================================================================

//======================================================================
// [FUNCTION]_[ud_parse_execution_report]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[extract one executionReport into a Command — x==TRADE fills (price/qty/maker/status/commission) + P3-e-ii terminal non-TRADE pass-through (EXPIRED/CANCELED -> venue_terminal, REJECTED -> rejection arm); CARRIES the open .E.0.10 parser findings (A4 commission non-authoritative, A5 side uncrosschecked — see the findings block below)]
// [REFERENCE]_[DECISION]_[D-123]
// [REFERENCE]_[TECH_DEBT]_[[TECH_DEBT-169] [TECH_DEBT-171]]
//======================================================================
// [CODE]
//======================================================================
// (f) D-479 / gate #3 G3-39. The parser's return becomes a THREE-valued code so the push site can
// tell "not for us" from "not a fill". The 8 existing `== 0` / `== 1` call sites stay valid by
// construction: DROP is 0 and OURS is 1, exactly what they already test.
enum UdParseRc { UD_PARSE_DROP = 0, UD_PARSE_OURS = 1, UD_PARSE_FOREIGN = 2 };

static inline int ud_parse_execution_report(const char* json, int len,
                                             Command* cmd_out,
                                             uint64_t* trade_id_out) {
    (void)len;
    // check event type
    char event_type[32] = {};
    binance_json_extract_str(json, "e", event_type, sizeof(event_type));
    if (strcmp(event_type, "executionReport") != 0) return 0;

    // Execution type — "TRADE" is a fill. P3-e-ii (D-446): a TERMINAL non-TRADE
    // report (the venue ENDED the order without a fill event: EXPIRED / CANCELED /
    // REJECTED) passes through so the OMS runs the terminal-incomplete disposition.
    // NEW / REPLACED / other working reports stay dropped (the REST ACK owns those).
    char exec_type[24] = {};
    binance_json_extract_str(json, "x", exec_type, sizeof(exec_type));
    const int is_trade    = (strcmp(exec_type, "TRADE") == 0);
    const int is_exp_canc = (strcmp(exec_type, "EXPIRED") == 0 ||
                             strcmp(exec_type, "CANCELED") == 0);
    const int is_rejected = (strcmp(exec_type, "REJECTED") == 0);
    if (!is_trade && !is_exp_canc && !is_rejected) return 0;

    // extract clientOrderId — should be "oms_<id>"
    char client_oid[64] = {};
    binance_json_extract_str(json, "c", client_oid, sizeof(client_oid));
    uint64_t oms_order_id = 0;
    if (strncmp(client_oid, "oms_", 4) == 0) {
        oms_order_id = strtoull(client_oid + 4, NULL, 10);
    }

    // extract exchange orderId
    char exchange_oid[32] = {};
    binance_json_extract_str(json, "i", exchange_oid, sizeof(exchange_oid));

    if (!is_trade) {
        // Terminal non-TRADE — OUR orders only (a foreign order's cancel is not our
        // event; passing id 0 would just spam the OMS surprise-fill arm). The stream
        // delivers this AFTER the TRADE legs it followed (venue-ordered), so booked
        // legs are already ahead of it in the SPSC ring — the OMS disposition sees
        // them applied. That ordering is WHY the terminal signal must come from THIS
        // stream when WS is active (a REST-raced terminal would fire the disposition
        // before the legs book — see the adapter's ws_active arm).
        if (oms_order_id == 0) return 0;
        char order_status[24] = {};
        binance_json_extract_str(json, "X", order_status, sizeof(order_status));
        memset(cmd_out, 0, sizeof(*cmd_out));
        cmd_out->type     = CMD_WS_FILL;
        cmd_out->order_id = oms_order_id;
        strncpy(cmd_out->result.exchange_id, exchange_oid,
                sizeof(cmd_out->result.exchange_id) - 1);
        if (is_rejected) {
            // Venue REJECTED (filters / balance / permissions): route to the OMS
            // rejection arm (ORDER_REJECTED + audit row + slot free) — deliberately
            // NO venue_terminal, so no auto re-submit: a structural reject would
            // loop tightly; the strategy re-evaluates the still-open position and
            // retries at its own cadence.
            cmd_out->result.success    = 0;
            cmd_out->result.error_code = -1;
            char reject_reason[24] = {};
            binance_json_extract_str(json, "r", reject_reason, sizeof(reject_reason));
            snprintf(cmd_out->result.error_message,
                     sizeof(cmd_out->result.error_message),
                     "WS x=REJECTED r=%s", reject_reason);
        } else {
            // EXPIRED / CANCELED: the venue_terminal disposition (booked legs stand;
            // a SELL's remainder re-submits; audit row rides the funnel).
            // order_complete from "X" — a report on an already-FILLED order keeps
            // complete=1 and the disposition arm stays cold.
            cmd_out->result.success        = 1;
            cmd_out->result.venue_terminal = 1;
            cmd_out->result.order_complete =
                (uint8_t)(strcmp(order_status, "FILLED") == 0);
        }
        *trade_id_out = 0;   // non-TRADE reports carry t=-1; no trade id
        return 1;
    }

    // fill data
    double fill_price = binance_json_extract_double(json, "L");
    double fill_qty   = binance_json_extract_double(json, "l");
    *trade_id_out     = (uint64_t)binance_json_extract_double(json, "t");

    // Phase 8 — maker/taker + order status + commission.
    // "m": Binance encodes booleans as bare true / false in JSON. The
    // existing extract_str returns the literal text — we check the first char.
    // Defensive default: missing "m" → is_maker=0 (taker, slightly overstates
    // fees, conservative) per master plan.
    char m_str[8] = {};
    binance_json_extract_str(json, "m", m_str, sizeof(m_str));
    int is_maker = (m_str[0] == 't' || m_str[0] == 'T') ? 1 : 0;

    // "X": order status. "FILLED" → terminal; anything else (including
    // "PARTIALLY_FILLED") is non-terminal. Defensive default: missing "X"
    // → order_complete=0 (assume partial — keeps order alive in OMS,
    // worst case we wait for next event to confirm).
    char order_status[24] = {};
    binance_json_extract_str(json, "X", order_status, sizeof(order_status));
    int order_complete = (strcmp(order_status, "FILLED") == 0) ? 1 : 0;

    // Commission: "n" amount + "N" asset. Recorded for audit; not the
    // authoritative fee number (Fee_Compute computes from cfg rates).
    double commission_amt = binance_json_extract_double(json, "n");
    char comm_asset[8] = {};
    binance_json_extract_str(json, "N", comm_asset, sizeof(comm_asset));

    // build the Command
    memset(cmd_out, 0, sizeof(*cmd_out));
    cmd_out->type     = CMD_WS_FILL;
    cmd_out->order_id = oms_order_id;
    cmd_out->result.success        = 1;
    cmd_out->result.avg_fill_price = fill_price;
    cmd_out->result.fill_qty       = fill_qty;
    cmd_out->result.error_code     = 0;
    strncpy(cmd_out->result.exchange_id, exchange_oid,
            sizeof(cmd_out->result.exchange_id) - 1);
    // Phase 8 fields
    cmd_out->result.is_maker       = (uint8_t)is_maker;
    cmd_out->result.order_complete = (uint8_t)order_complete;
    cmd_out->result.commission     = commission_amt;
    strncpy(cmd_out->result.commission_asset, comm_asset,
            sizeof(cmd_out->result.commission_asset) - 1);

    // A TRADE we fully decoded but whose clientOrderId is not an `oms_<id>` of ours: the venue is
    // reporting someone ELSE's fill on this account (a manual web order, another bot). The Command
    // is returned DECODED anyway so the router can log what actually happened — pushing it would
    // book a stranger's fill into our ledger, and dropping it silently would hide that the account
    // is being traded from elsewhere. Both are wrong; naming it is right.
    return (oms_order_id == 0) ? UD_PARSE_FOREIGN : UD_PARSE_OURS;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Extracts one executionReport JSON event into a Command.
// Returns 1 for a fill (x == "TRADE") OR a terminal non-TRADE on OUR order
// (x == EXPIRED/CANCELED -> venue_terminal command; x == REJECTED -> success=0
// rejection command); 0 for anything else (working reports, foreign orders).
//
// Relevant fields from the Binance docs:
//   "e": "executionReport"   — event type
//   "x": "TRADE"             — execution type (TRADE = fill)
//   "X": "FILLED"            — order status
//   "c": "oms_123"           — clientOrderId (our idempotency key)
//   "i": 12345               — exchange orderId
//   "L": "60123.45"          — last executed price
//   "l": "0.001"             — last executed quantity
//   "n": "0.06"              — commission amount
//   "N": "BNB"               — commission asset
//   "t": 98765               — trade ID (for deduplication)
//   "T": 1234567890123       — transaction time (ms)
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// [[.E.0.10 adversarial hunt] [KNOWN OPEN CAPITAL FINDINGS — read before editing]]
//----------------------------------------------------------------------
// A2 (was HIGH, TECH_DEBT-169) — OMS HALF FIXED at E.1.3 P3-e-i: filled_qty
//   now ACCUMULATES per leg and the slot frees only on a TERMINAL state, so
//   multi-partial orders book whole. Venue "z" (cumulative) stays unparsed BY
//   DESIGN on this WS path — "l" (the leg) is the increment the OMS wants; "z"
//   re-enters at E.1.4's GetStatus reconcile as the cross-check total.
// A4 (MED→HIGH on BNB-pay, TECH_DEBT-169): the "n"/"N" commission parsed
//   here is recorded but NOT booked authoritatively (Fee_Compute fabricates
//   notional×rate downstream); the reconcile path drops commission entirely.
//   Contract-to-be: carry venue commission + asset source-exact on BOTH
//   the WS and reconcile paths (D-123).
// A5 (MED, TECH_DEBT-171): venue "S" (side) is never parsed/cross-checked
//   vs the local order type — a slot-decode slip books a buy as a sell
//   with no guard (Knight-shaped).
// Fixes ride .E.0.10 STOPs + the D-123 decimal-OrderResult rework — this
// block is the at-site pointer, the disposition register is the SSoT.
//======================================================================
// [END_FUNCTION]_[ud_parse_execution_report]
//======================================================================

//======================================================================
// [FUNCTION]_[ud_route_command]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CAPITAL_BEARING] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the WS fill push site as an explicit ROUTER: SWITCHES on the parser's UdParseRc so a FOREIGN report can never be pushed, and sends OURS through the never-drop bounded push that goes LOUD-FATAL + requests the GLOBAL kill rather than dropping a fill]
// [REFERENCE]_[DECISION]_[D-479]
//======================================================================
// [CODE]
//======================================================================
// A SWITCH, not a truthiness test — and that is the whole reason this is a named function. Both
// OURS(1) and FOREIGN(2) are truthy, so `if (parse(...))` would push a stranger's fill into our
// ledger. The shape makes the mistake unrepresentable rather than merely discouraged.
static inline void ud_route_command(BinanceUserDataState* s, int rc, const Command& cmd,
                                    uint64_t trade_id, const char* raw_json, int raw_len) {
    if (rc == UD_PARSE_FOREIGN) {
        s->foreign_fills.fetch_add(1, std::memory_order_relaxed);
        // Re-extract the RAW "c" on this rare path so `web_abc` (a manual order from the Binance
        // web UI) and `oms_garbage` (our own id scheme, malformed) are DISTINGUISHABLE in the log.
        // The parsed Command cannot carry it: it zeroed the id precisely because it did not match.
        char raw_c[64] = {};
        binance_json_extract_str(raw_json, "c", raw_c, sizeof(raw_c));
        (void)raw_len;
        tt::Health_Log(tt::HEALTH_WARN, "ws_foreign_fill", -1,
                       "clientOrderId=%s venue_id=%s price=%.8f qty=%.8f commission=%.8f %s "
                       "trade_id=%llu — this account is being traded from OUTSIDE this engine; "
                       "the fill was NOT booked (it is not ours to book)",
                       raw_c[0] ? raw_c : "(none)",
                       cmd.result.exchange_id[0] ? cmd.result.exchange_id : "(none)",
                       cmd.result.avg_fill_price, cmd.result.fill_qty, cmd.result.commission,
                       cmd.result.commission_asset[0] ? cmd.result.commission_asset : "(none)",
                       (unsigned long long)trade_id);
        return;
    }
    // OURS. Written FLIP-READY (g): at commit 4 there is deliberately NO lane divert — the interim
    // central walk drains all 16 rings and ProcessFillCommand's verify ignores an unregistered lane,
    // so `registered_count` is not read here. At the flip, a report on an unregistered lane is OUR
    // PRIOR SESSION's order reaching its terminal, which gets its own `stale_lane_fills` counter —
    // never `foreign_fills`, because it IS ours. (G3-39's "boot-stable int" premise was false by
    // ordering: BinanceUserData_Start precedes the registration loop.)
    //
    // The double-probe gives the retry counter without a `waited` out-param on the commit-2
    // primitive — an out-param there would be a stranded write on every fast-path push (Class 62).
    if (OMS_CmdRingsPush(s->ws_rings, cmd)) {
        s->fills_received.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s->ws_push_retries.fetch_add(1, std::memory_order_relaxed);
    if (OMS_CmdRingsPushOrTrip(s->ws_rings, cmd, OMS_RING_PUSH_BUDGET_CYCLES,
                               /*abort=*/nullptr, s->kill_trip_word,
                               KTS_WS_RING_FULL, &s->ws_push_fatal)) {
        s->fills_received.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The record is already durable (OMS_CmdRingsPushOrTrip wrote it and requested the GLOBAL kill).
    // Nothing further here: the fill is unbooked, the engine is stopping, and reconciliation from
    // venue truth is the only correct recovery.
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ud_route_command]
//======================================================================

//======================================================================
// [FUNCTION]_[ud_consume_frame]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CAPITAL_BEARING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the per-frame body of the fill stream, templated over the Io transport so the suite drives the SAME code the venue does — read one frame, answer a ping with the masked echo, disconnect on a close/desync, parse an executionReport and push CMD_WS_FILL into the OMS ring. Returns 1 = still connected, 0 = the caller must drop the connection and reconnect]
//======================================================================
// [CODE]
//======================================================================
// EXTRACTED at the 2026-09-05 leaf's commit 4, mirroring `depth_consume_frame` (commit 2). The reason is
// the same and the stakes are higher: while this body was welded to a live `SSL*`, NOTHING here could be
// tested — not the pong, not the desync arms, and not the ring push that BOOKS A FILL. A fill dropped
// because the ring was full is a capital event, and it was reachable only in production. Now every arm is
// suite-drivable through a byte cursor (`WsBufIo`), with the SSL handle NULL.
//
// Returns 1 = still connected, 0 = disconnect (the caller breaks its read loop, runs `ud_ws_close`, and
// reconnects with its own backoff — the cleanup and retry policy stay in the thread where they belong).
template <class Io>
static inline int ud_consume_frame(BinanceUserDataState* s, Io& io, char* frame_buf, int frame_cap) {
    int opcode = 0, fin = 0;
    // frame_cap is sizeof(buf)-1: ws_read_frame bounds the payload at max_len and NUL-terminates at out[len].
    const int payload_len = ws_read_frame(io, frame_buf, frame_cap, &opcode, &fin);
    if (payload_len < 0) {
        // Every negative return leaves the stream DESYNCED (the payload was not consumed), so the only
        // correct response to any of them is to drop the connection — never "skip and continue".
        fprintf(stderr, "[UserData] %s, reconnecting\n",
                payload_len == WS_READ_TOO_LARGE ? "frame larger than the frame buffer"
              : payload_len == WS_READ_PROTOCOL  ? "RFC 6455 control-frame violation (>125 B or fragmented)"
                                                 : "frame read error");
        return 0;
    }

    // ping -> MASKED payload-echo pong (RFC 6455 §5.1 + §5.5.3). A failed write means the peer is gone:
    // keeping the loop alive on a dead socket is how the depth stream stalled for 40+ min looking healthy.
    if (opcode == 0x9) {
        if (!ws_send_pong(io, frame_buf, payload_len)) {
            fprintf(stderr, "[UserData] pong write failed, reconnecting\n");
            return 0;
        }
        return 1;
    }
    if (opcode == 0x8) {
        // The server's close: a 2-byte big-endian code + a UTF-8 reason. Printed because a silent close
        // arm is exactly what made the 2026-09-05 depth stall invisible for 40 minutes.
        const int code = (payload_len >= 2)
                       ? ((((unsigned char)frame_buf[0]) << 8) | (unsigned char)frame_buf[1]) : 0;
        fprintf(stderr, "[UserData] server close code=%d reason=\"%.*s\"\n",
                code, (payload_len > 2) ? payload_len - 2 : 0, (payload_len > 2) ? frame_buf + 2 : "");
        return 0;
    }
    if (opcode != 0x1) return 1;                 // binary / pong / continuation: ignored

    s->events_received.fetch_add(1, std::memory_order_relaxed);

    Command cmd;
    uint64_t trade_id = 0;
    const int rc = ud_parse_execution_report(frame_buf, payload_len, &cmd, &trade_id);
    if (rc != UD_PARSE_DROP) {
        ud_route_command(s, rc, cmd, trade_id, frame_buf, payload_len);
    }
    return 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ud_consume_frame]
//======================================================================

//======================================================================
// [FUNCTION]_[ud_ws_thread]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the WS thread — listen-key -> WSS connect -> SSL_pending-aware poll loop -> parse + SPSC push (sole producer); keepalive-failure + disconnect reconnect with Notify alert]
//======================================================================
// [CODE]
//======================================================================
static inline void ud_ws_thread(BinanceUserDataState* s) {
    while (s->shutdown_requested.load(std::memory_order_acquire) == 0) {
        // 1. Obtain listen key
        if (!ud_obtain_listen_key(s)) {
            fprintf(stderr, "[UserData] failed to obtain listen key, retrying in 5s\n");
            for (int i = 0; i < 50 && !s->shutdown_requested.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        s->keepalive_failed.store(0, std::memory_order_relaxed);

        // 2. Connect WSS
        // rcv_timeout 0 = the pre-migration blocking behaviour; a timeout on the FILL socket is a
        // capital-path liveness decision, tracked as TECH_DEBT-345 with the trade socket, not slipped in here.
        s->sockfd = ws_tcp_connect(s->ws_host, 443, 0);
        if (s->sockfd < 0) {
            fprintf(stderr, "[UserData] TCP connect failed to %s:443, retrying in 5s\n", s->ws_host);
            for (int i = 0; i < 50 && !s->shutdown_requested.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (ws_ssl_setup(&s->ssl_ctx, &s->ssl, s->sockfd, s->ws_host) < 0) {
            fprintf(stderr, "[UserData] TLS setup failed, retrying in 5s\n");
            close(s->sockfd); s->sockfd = -1;
            for (int i = 0; i < 50 && !s->shutdown_requested.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        char ws_path[256];
        snprintf(ws_path, sizeof(ws_path), "/ws/%s", s->listen_key);
        WsSslIo hio{s->ssl};
        if (ws_handshake(hio, s->ws_host, ws_path) < 0) {
            fprintf(stderr, "[UserData] WS handshake failed (no 101), retrying in 5s\n");
            ud_ws_close(s);
            for (int i = 0; i < 50 && !s->shutdown_requested.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        s->connected = 1;
        s->ws_connected.store(1, std::memory_order_relaxed);
        s->reconnect_count.fetch_add(1, std::memory_order_relaxed);
        fprintf(stderr, "[UserData] WSS connected to %s%s\n", s->ws_host, ws_path);

        // 3. Read loop
        char frame_buf[4096];
        WsSslIo io{s->ssl};
        while (s->shutdown_requested.load(std::memory_order_acquire) == 0) {
            // check if keepalive failed (forces reconnect)
            if (s->keepalive_failed.load(std::memory_order_relaxed)) {
                fprintf(stderr, "[UserData] keepalive failed, reconnecting\n");
                break;
            }

            // check if data is available (poll with 200ms timeout so we
            // can check shutdown_requested periodically)
            // IMPORTANT: check SSL_pending first — OpenSSL may have buffered
            // decrypted data that poll() won't see (same pattern as
            // BinanceStream_Poll in BinanceCrypto.hpp).
            if (SSL_pending(s->ssl) == 0) {
                struct pollfd pfd;
                pfd.fd = s->sockfd;
                pfd.events = POLLIN;
                int ready = poll(&pfd, 1, 200);
                if (ready <= 0) continue;
                if (pfd.revents & (POLLERR | POLLHUP)) {
                    fprintf(stderr, "[UserData] socket error/hangup\n");
                    break;
                }
            }

            // The whole per-frame body lives in ud_consume_frame (above) so the SUITE can drive it.
            if (!ud_consume_frame(s, io, frame_buf, (int)sizeof(frame_buf) - 1)) break;
        }

        // disconnected — clean up and loop back
        ud_ws_close(s);
        if (s->shutdown_requested.load(std::memory_order_acquire)) break;

        fprintf(stderr, "[UserData] disconnected, reconnecting in 2s\n");
        // Phase 8b: alert. Cooldown collapses the keepalive/frame-read/disconnect
        // log triplet into one alert per cooldown window — this is the convergence
        // point that always runs on disconnect, so a single Send here is enough.
        if (g_notify) {
            Notify_Send(g_notify, NOTIFY_WARN, NK_DISCONNECT_USERDATA,
                        "Binance user-data WS disconnected",
                        "Reconnecting automatically in 2 seconds. "
                        "Investigate if disconnects are frequent or persistent.");
        }
        for (int i = 0; i < 20 && !s->shutdown_requested.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ud_ws_thread]
//======================================================================

//======================================================================
// [FUNCTION]_[ud_keepalive_thread]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [CONCURRENCY]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[25-min listen-key refresh (Binance expires at 60) + per-cycle clock sync — retry-once then signal reconnect; 5-consecutive-failure circuit breaker disables WS fills]
//======================================================================
// [CODE]
//======================================================================
static inline void ud_keepalive_thread(BinanceUserDataState* s) {
    int consecutive_failures = 0;
    while (s->shutdown_requested.load(std::memory_order_acquire) == 0) {
        // sleep 25 minutes, checking shutdown every 100ms
        for (int i = 0; i < 15000; ++i) {
            if (s->shutdown_requested.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (s->listen_key[0] == '\0') continue;
        if (!s->ws_connected.load(std::memory_order_relaxed)) {
            consecutive_failures = 0;  // reset on disconnect (WS thread reconnects)
            continue;
        }

        // Try keepalive PUT
        if (ud_keepalive_listen_key(s)) {
            fprintf(stderr, "[UserData] listen key refreshed\n");
            consecutive_failures = 0;
            // Sync clock every keepalive cycle (~25 min) to prevent
            // timestamp drift on signed requests.
            BinanceOrderAPI_SyncClock(&s->rest_api);
            continue;
        }

        // First failure: retry once after 5s
        fprintf(stderr, "[UserData] keepalive failed, retrying in 5s\n");
        for (int i = 0; i < 50; ++i) {
            if (s->shutdown_requested.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (ud_keepalive_listen_key(s)) {
            fprintf(stderr, "[UserData] keepalive retry succeeded\n");
            consecutive_failures = 0;
            continue;
        }

        // Second failure: signal WS thread to reconnect (which recreates the key)
        consecutive_failures++;
        fprintf(stderr, "[UserData] keepalive retry failed (%d consecutive), "
                         "signaling reconnect\n", consecutive_failures);
        s->keepalive_failed.store(1, std::memory_order_relaxed);

        // Circuit breaker: after 5 consecutive failures across reconnects,
        // something is fundamentally wrong (API key revoked, network down,
        // Binance outage). Stop trying.
        if (consecutive_failures >= 5) {
            fprintf(stderr, "[UserData] CRITICAL: 5 consecutive keepalive failures, "
                             "circuit breaker tripped — WS fills disabled\n");
            s->shutdown_requested.store(1, std::memory_order_release);
            return;
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[ud_keepalive_thread]
//======================================================================

//======================================================================
// [FUNCTION]_[BinanceUserData_Init]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [LIVE_TRADING] [BOOT_TIME]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the lifecycle family (Start / Shutdown ride) — Init wires state + the dedicated REST instance (no connect); Start spawns the thread pair; Shutdown joins + cleans]
//======================================================================
// [CODE]
//======================================================================
static inline int BinanceUserData_Init(BinanceUserDataState* s,
                                        const char* ws_host,
                                        const char* rest_host,
                                        const char* api_key,
                                        const char* api_secret,
                                        const char* symbol,
                                        OmsCmdRings* ws_rings,
                                        std::atomic<uint32_t>* kill_trip_word) {
    s->sockfd    = -1;
    s->ssl_ctx   = NULL;
    s->ssl       = NULL;
    s->connected = 0;
    s->listen_key[0] = '\0';
    strncpy(s->ws_host, ws_host, sizeof(s->ws_host)-1);
    s->ws_host[sizeof(s->ws_host)-1] = '\0';
    strncpy(s->rest_host, rest_host, sizeof(s->rest_host)-1);
    s->rest_host[sizeof(s->rest_host)-1] = '\0';
    s->ws_rings = ws_rings;
    s->kill_trip_word = kill_trip_word;
    s->foreign_fills.store(0, std::memory_order_relaxed);
    s->ws_push_retries.store(0, std::memory_order_relaxed);
    s->ws_push_fatal.store(0, std::memory_order_relaxed);

    s->shutdown_requested.store(0, std::memory_order_relaxed);
    s->keepalive_failed.store(0, std::memory_order_relaxed);
    s->ws_connected.store(0, std::memory_order_relaxed);
    s->fills_received.store(0, std::memory_order_relaxed);
    s->events_received.store(0, std::memory_order_relaxed);
    s->reconnect_count.store(0, std::memory_order_relaxed);

    // Initialize the dedicated REST API instance for listen key management.
    // This instance is ONLY used by the WS thread and keepalive thread
    // (serialized — WS thread only does REST during reconnect, keepalive
    // only fires when WS is connected). No concurrent use.
    if (!BinanceOrderAPI_Init(&s->rest_api, rest_host, api_key, api_secret, symbol)) {
        fprintf(stderr, "[UserData] failed to init REST API instance\n");
        return 0;
    }
    return 1;
}

static inline void BinanceUserData_Start(BinanceUserDataState* s) {
    s->ws_thread        = std::thread(ud_ws_thread, s);
    s->keepalive_thread = std::thread(ud_keepalive_thread, s);
    fprintf(stderr, "[UserData] WS + keepalive threads started\n");
}

static inline void BinanceUserData_Shutdown(BinanceUserDataState* s) {
    s->shutdown_requested.store(1, std::memory_order_release);
    if (s->ws_thread.joinable())        s->ws_thread.join();
    if (s->keepalive_thread.joinable()) s->keepalive_thread.join();
    ud_ws_close(s);
    BinanceOrderAPI_Cleanup(&s->rest_api);
    fprintf(stderr, "[UserData] shutdown complete (fills=%llu events=%llu reconnects=%llu)\n",
            (unsigned long long)s->fills_received.load(),
            (unsigned long long)s->events_received.load(),
            (unsigned long long)s->reconnect_count.load());
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//----------------------------------------------------------------------
// Init does NOT connect — call BinanceUserData_Start to spawn the threads
// and begin streaming.
//
// ws_host: the websocket host ("stream.binance.vision" for testnet,
//          "stream.binance.com" for production)
// rest_host: the REST host for listen key calls (same as adapter REST host)
// ws_rings: pointer to the OMS's per-node WS fill rings (routed by the id's node lane)
// kill_trip_word: &agg.kill_trip_request — APPENDED at the signature TAIL per TD-288 (a
//   mid-signature insertion silently re-binds every existing positional argument)
//======================================================================
// [END_FUNCTION]_[BinanceUserData_Init]
//======================================================================

}  // namespace tt
