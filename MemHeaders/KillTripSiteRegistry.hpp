#pragma once
//======================================================================================================
// [FILE]_[KillTripSiteRegistry.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [CONCURRENCY] [LIVE_TRADING] [OMS_DRAINER]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the GLOBAL fatal-trip SITES of AggregatorState::kill_trip_request — the X-macro registry behind the word's upper lanes (D-479 as amended by gate #3, G3-1/G3-2): one row per producer-side site that can find a fill ring full past its bounded push and must halt the whole engine LOUDLY rather than drop venue money; generates the KTS_* enum, the count, the name table the durable Health_Log record + the OEVT_RING_FULL_FATAL marker row carry, and the lane masks]
//======================================================================================================
// WHY a registry and not four #defines: the site NAME is what the durable record and the
// event-log marker row persist ("FATAL:WS_RING_FULL" in OrderEvent::reason[32] — the fit is
// static_assert-pinned per row at the append site in EngineCommon.hpp), so the names are H21
// append-only identifiers — a registry with explicit values enrolls in tools/identifier_ledger.txt
// as one row (enum:KillTripSite) and the H21 guard REDs a renumber/rename/drop. The lane BIT is
// in-memory only (the trip word is never persisted); the name is the wire form.
//
// The lane layout of kill_trip_request (uint32, AggregatorState line 0):
//   bits  0..15   per-NODE trip lanes (bit n = node n) — the (L)(2) drift-trip->command vehicle,
//                 landed early: any thread fetch_or's a node's lane; the composer replays the
//                 per-node trip body at compose step 0a (idempotent on an already-tripped node).
//   bits 16..31   GLOBAL trip lanes, one per KTS_* site below — the composer calls
//                 EventLoop_KillSwitchTrip (its first live caller), notifies, and appends the
//                 OEVT_RING_FULL_FATAL marker row. RESTART-ONLY by design (D-481 / TD-328): the
//                 GLOBAL kill bit has no runtime reset path — an unbooked fill corrupts the ledger
//                 every node's eval reads, and resuming on top of it is the Knight shape.
//
// Adding a site = ONE row here (+ the producer wiring that requests it). The count is pinned
// <= 16 because the word has sixteen global lanes.
//======================================================================================================

#include <stdint.h>

namespace tt {

//======================================================================
// [REGISTRY]_[FOREACH_KILL_TRIP_SITE]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [CONCURRENCY] [LIVE_TRADING]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the complete set of GLOBAL fatal-trip sites — X(name, explicit_value, doc); value = the global lane index (bit 16+value of kill_trip_request); the name is persisted in the OEVT_RING_FULL_FATAL marker row + the Health_Log CRITICAL record, so rows are H21 append-only (enum:KillTripSite in the identifier ledger)]
// [COLUMN]_[name]_[UPPERCASE token; produces KTS_<name>]
// [COLUMN]_[value]_[explicit global lane index 0..15; append-only, never renumbered]
// [COLUMN]_[doc_string]_[which producer + which ring; the policy row of D-479's per-site table]
// [REFERENCE]_[DECISION]_[D-479]
//======================================================================
// [CODE]
//======================================================================
#define FOREACH_KILL_TRIP_SITE(X)                                                                        \
    /* The WS user-data parser thread: a per-node WS result ring stayed full past the bounded push   */ \
    /* (3b(ii) commit 4 wires the request; the ring family lands there).                             */ \
    X(WS_RING_FULL,          0, "WS user-data thread: a per-node result ring stayed full past the bounded push") \
    /* The REST adapter worker: a per-node REST result ring stayed full past the bounded push. The   */ \
    /* full-cause is consumer LIVENESS only (worker_count == 1 serializes pushes; V3).                */ \
    X(REST_RING_FULL,        1, "REST adapter worker: a per-node result ring stayed full past the bounded push") \
    /* The paper-synth push (producer == consumer thread): structurally unreachable while          */ \
    /* OMS_RESULT_RING_PER_NODE >= MAX_INFLIGHT_ORDERS (paper-only pin, G3-4) — on the impossible   */ \
    /* full: immediate fatal, NEVER a spin (a self-wait), and the order slot is freed.               */ \
    X(PAPER_SYNTH_RING_FULL, 2, "paper-synth push found its ring full (structurally unreachable; fatal, never spin)") \
    /* The drain-bucket stage (D-479 member #5, the M9 miss): 257th same-direction command in one   */ \
    /* drainer cycle — REST 16 ACKs + WS 256 legs = 272 > 256 is reachable (V13; punch-7).           */ \
    X(BUCKET_OVERFLOW,       3, "drain-bucket stage overflow: 257th same-direction command in one cycle")

//----------------------------------------------------------------------
// [SECTION]_[auto-generated consumers — enum + count + name table + lane masks]
//----------------------------------------------------------------------
#define X_GEN_KTS_ENUM(name, val, doc) KTS_##name = val,
enum KillTripSite : uint8_t {
    FOREACH_KILL_TRIP_SITE(X_GEN_KTS_ENUM)
};
#undef X_GEN_KTS_ENUM

#define X_GEN_KTS_COUNT_ONE(name, val, doc) +1
#define KILL_TRIP_SITE_COUNT (0 FOREACH_KILL_TRIP_SITE(X_GEN_KTS_COUNT_ONE))
static_assert(KILL_TRIP_SITE_COUNT <= 16,
              "kill_trip_request holds SIXTEEN global lanes (bits 16..31) — a 17th site needs a "
              "wider word, decided deliberately (H21: the lane indices are append-only)");

// The persisted spelling of a site (the OEVT_RING_FULL_FATAL marker row's reason + the
// Health_Log CRITICAL record). A switch, deliberately: this runs on the FATAL path only (once
// per trip, never per tick — H20's rare-cold exception), and a switch tolerates a gap in the
// value space where a dense table would not. An out-of-range value names itself so a corrupt
// lane never reads as a real site.
static inline const char* KillTripSite_Name(uint8_t site) {
    switch (site) {
#define X_GEN_KTS_NAME(name, val, doc) case val: return #name;
        FOREACH_KILL_TRIP_SITE(X_GEN_KTS_NAME)
#undef X_GEN_KTS_NAME
        default: return "UNKNOWN_SITE";
    }
}

// Lane masks over the uint32 trip word. NODE lanes: bit n. GLOBAL lanes: bit 16 + site.
// Both index arguments are masked to their 16-lane half BEFORE the shift so a corrupt index
// (a node >= 16, a site >= 16) can never become a >= 32-bit shift (UB). What the mask does
// NOT give you is attribution: a corrupt site whose low 4 bits collide with a registered row
// (17 → REST_RING_FULL) is consumed AS that row, and KillTripSite_Name names the UNMASKED
// value at the record but the MASKED lane at the marker. The producer passes a typed
// KillTripSite, so a corrupt value has no live source; the consumer consumes an unregistered
// NODE lane with a stderr line, and an out-of-range SITE lane names UNKNOWN_SITE
// (V-1 L-4; wording corrected 2026-09-04 — AR-8 N-5 refuted "loud, never undefined").
#define KILL_TRIP_SHIFT_GLOBAL      16u
#define KILL_TRIP_MASK_NODES        0x0000FFFFu
#define KILL_TRIP_MASK_GLOBAL       0xFFFF0000u
#define KILL_TRIP_LANE_NODE(n)      (1u << ((unsigned)(n) & 15u))
#define KILL_TRIP_LANE_GLOBAL(site) (1u << (KILL_TRIP_SHIFT_GLOBAL + ((unsigned)(site) & 15u)))
static_assert((KILL_TRIP_MASK_NODES & KILL_TRIP_MASK_GLOBAL) == 0u &&
              (KILL_TRIP_MASK_NODES | KILL_TRIP_MASK_GLOBAL) == 0xFFFFFFFFu,
              "the two lane halves partition the trip word exactly");
//======================================================================
// [END_CODE]
//======================================================================
// [END_REGISTRY]_[FOREACH_KILL_TRIP_SITE]
//======================================================================

}  // namespace tt
