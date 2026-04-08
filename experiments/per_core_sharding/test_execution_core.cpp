// test_execution_core.cpp — smoke test for the ExecutionCore<F> hot path
//
// What this validates:
//   1. All headers (Tick, TradeEvent, GateParameters, SPSCRing, ExecutionCore)
//      compile cleanly together with -O3 -Wall -Wextra
//   2. Init produces clean state (permission=0, active=0, entry_price=0)
//   3. Permission=0 prevents entry even when BG would fire
//   4. Permission=1 + BG fires → active=1, entry_price set, entry event pushed
//   5. Active + SG fires → active=0, exit event pushed
//   6. Permission revoked mid-trade → existing position still exits via SG
//   7. Per-tick latency (single-threaded) measured via rdtsc
//
// What it does NOT validate (deferred to phase 03 / phase 12):
//   - Branchless verification via disassembly (phase 03 acceptance criterion)
//   - Multi-threaded behavior with real producer/consumer (phase 12)
//   - End-to-end with real strategies (phase 06+)

#include "CoreFrameworks/ExecutionCore.hpp"
#include "common/rdtsc.hpp"

#include <cstdint>
#include <cstdio>

using namespace tt;
using namespace experiment;

static int failures = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);     \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

// Helper: build a tick with a given price (volume defaulted to a fixed value
// large enough to satisfy the volume gate when needed).
static Tick<64> make_tick(double price, uint64_t timestamp, uint64_t seq) {
    Tick<64> t;
    t.price = FPN_FromDouble<64>(price);
    t.volume = FPN_FromDouble<64>(1000.0);
    t.timestamp = timestamp;
    t.sequence = seq;
    return t;
}

// Helper: configure the core to enter when price < 60000, exit when price >=
// 60100 (TP) or <= 59900 (SL). Entry threshold below 60000, narrow band.
//
// Phase 05 update: parameters now go through ExecutionCore_SetParameters which
// pushes them through the triple-buffered ParameterSlot. Builds a local
// GateParameters struct, fills it, then writes via the helper.
static void configure_test_strategy(ExecutionCore<64>* core) {
    GateParameters<64> p;
    GateParameters_Init(&p);
    p.bg_price_threshold   = FPN_FromDouble<64>(60000.0);
    p.bg_volume_threshold  = FPN_Zero<64>();  // any volume
    p.sg_take_profit_price = FPN_FromDouble<64>(60100.0);
    p.sg_stop_loss_price   = FPN_FromDouble<64>(59900.0);
    p.trade_size           = FPN_FromDouble<64>(0.01);
    p.strategy_id          = STRATEGY_SIMPLE_DIP;
    p.flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    ExecutionCore_SetParameters(core, p);
}

static void test_init_state() {
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);

    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 7, &tick_ring);

    EXPECT(core.permission == 0, "init: permission should be 0");
    EXPECT(core.active == 0, "init: active should be 0");
    EXPECT(core.core_id == 7, "init: core_id should be set");
    GateParameters<64> init_params;
    ParameterSlot_Read(&core.param_slot, &init_params);
    EXPECT(init_params.strategy_id == STRATEGY_NONE, "init: strategy NONE");
    EXPECT(core.tick_ring == &tick_ring, "init: tick_ring pointer set");
    EXPECT(SPSCRing_Depth(&core.event_ring) == 0, "init: event ring empty");
}

static void test_no_trade_without_permission() {
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tick_ring);
    configure_test_strategy(&core);
    // permission stays 0

    // Tick at price 59000 — well below the BG threshold of 60000, BG would fire.
    Tick<64> tick = make_tick(59000.0, 1000000, 1);
    ExecutionCore_Tick(&core, tick);

    EXPECT(core.active == 0, "no permission → no entry even when BG fires");
    EXPECT(SPSCRing_Depth(&core.event_ring) == 0, "no permission → no event");
}

static void test_entry_when_permitted() {
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tick_ring);
    configure_test_strategy(&core);
    core.permission = 1;

    Tick<64> tick = make_tick(59000.0, 1000000, 1);
    ExecutionCore_Tick(&core, tick);

    EXPECT(core.active == 1, "permission + BG fires → active=1");
    EXPECT(SPSCRing_Depth(&core.event_ring) == 1, "entry produces one event");

    TradeEvent<64> event;
    EXPECT(SPSCRing_TryPop(&core.event_ring, &event), "pop entry event");
    EXPECT(event.type == TRADE_EVENT_ENTRY, "event type should be ENTRY");
    EXPECT(event.core_id == 0, "event has correct core_id");
    EXPECT(event.timestamp == 1000000, "event timestamp matches tick");
    EXPECT(FPN_ToDouble(event.price) == 59000.0, "event price matches tick");
    EXPECT(FPN_ToDouble(core.entry_price) == 59000.0, "entry_price recorded");
}

static void test_no_double_entry() {
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tick_ring);
    configure_test_strategy(&core);
    core.permission = 1;

    // First tick: enter
    ExecutionCore_Tick(&core, make_tick(59000.0, 1000000, 1));
    EXPECT(core.active == 1, "first entry");

    // Second tick: BG would fire (price < 60000) but core is already active.
    // Price must be ABOVE SL (59900) and BELOW TP (60100) so SG doesn't fire either.
    // Use 59950: below BG threshold (60000) so BG_fires=1, above SL, below TP.
    ExecutionCore_Tick(&core, make_tick(59950.0, 1000001, 2));
    EXPECT(core.active == 1, "still in trade, no double entry");
    EXPECT(SPSCRing_Depth(&core.event_ring) == 1, "only one event so far");
}

static void test_exit_on_tp() {
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 3, &tick_ring);
    configure_test_strategy(&core);
    core.permission = 1;

    // Enter at 59000
    ExecutionCore_Tick(&core, make_tick(59000.0, 1000000, 1));
    // Drain entry event
    TradeEvent<64> entry_event;
    SPSCRing_TryPop(&core.event_ring, &entry_event);

    // Tick at 60100 — hits TP exactly
    ExecutionCore_Tick(&core, make_tick(60100.0, 1000001, 2));
    EXPECT(core.active == 0, "TP hit → exit");
    EXPECT(SPSCRing_Depth(&core.event_ring) == 1, "exit produces one event");

    TradeEvent<64> exit_event;
    EXPECT(SPSCRing_TryPop(&core.event_ring, &exit_event), "pop exit event");
    EXPECT(exit_event.type == TRADE_EVENT_EXIT, "event type should be EXIT");
    EXPECT(exit_event.core_id == 3, "event has correct core_id");
    EXPECT(FPN_ToDouble(exit_event.price) == 60100.0, "exit price matches tick");
}

static void test_exit_on_sl() {
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tick_ring);
    configure_test_strategy(&core);
    core.permission = 1;

    ExecutionCore_Tick(&core, make_tick(59000.0, 1000000, 1));
    TradeEvent<64> entry_event;
    SPSCRing_TryPop(&core.event_ring, &entry_event);

    // Tick at 59900 — hits SL exactly
    ExecutionCore_Tick(&core, make_tick(59900.0, 1000001, 2));
    EXPECT(core.active == 0, "SL hit → exit");

    TradeEvent<64> exit_event;
    EXPECT(SPSCRing_TryPop(&core.event_ring, &exit_event), "pop exit event");
    EXPECT(exit_event.type == TRADE_EVENT_EXIT, "type should be EXIT");
}

static void test_permission_revoke_during_trade() {
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tick_ring);
    configure_test_strategy(&core);
    core.permission = 1;

    // Enter
    ExecutionCore_Tick(&core, make_tick(59000.0, 1000000, 1));
    EXPECT(core.active == 1, "entered");
    TradeEvent<64> drop;
    SPSCRing_TryPop(&core.event_ring, &drop);

    // Controller revokes permission
    core.permission = 0;

    // Tick that would cause neither TP nor SL: position holds
    ExecutionCore_Tick(&core, make_tick(60000.0, 1000001, 2));
    EXPECT(core.active == 1, "still active during permission revoke (no exit signal)");

    // Tick that hits TP: position exits even with permission revoked (exits don't check permission)
    ExecutionCore_Tick(&core, make_tick(60100.0, 1000002, 3));
    EXPECT(core.active == 0, "TP exit fires regardless of permission");
}

static void measure_tick_latency(uint64_t tsc_hz) {
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tick_ring);
    configure_test_strategy(&core);
    core.permission = 1;

    // Use a price that won't trigger BG (above threshold) so no events are
    // pushed during the steady-state measurement. This isolates the gate
    // evaluation cost from the rare event push branch.
    Tick<64> tick = make_tick(60050.0, 1000000, 1);  // above BG, below TP, above SL

    constexpr int ITERS = 100'000;
    uint64_t total = 0;
    uint64_t min = UINT64_MAX;

    for (int i = 0; i < ITERS; ++i) {
        tick.timestamp = 1000000 + (uint64_t)i;
        uint64_t s = rdtsc_start();
        ExecutionCore_Tick(&core, tick);
        uint64_t e = rdtsc_end();
        uint64_t cycles = e - s;
        if (cycles < min) min = cycles;
        total += cycles;
    }

    double avg = (double)total / ITERS;
    printf("\n--- ExecutionCore_Tick latency (steady state, no fire) ---\n");
    printf("min %lu cycles (%.1f ns), avg %.1f cycles (%.1f ns)\n",
           min, cycles_to_ns(min, tsc_hz),
           avg, cycles_to_ns((uint64_t)avg, tsc_hz));
    printf("Note: includes ~25ns rdtsc overhead floor.\n");
    printf("Subtract floor for actual hot-path cost: %.1f ns min, %.1f ns avg\n",
           cycles_to_ns(min, tsc_hz) - 25.0,
           cycles_to_ns((uint64_t)avg, tsc_hz) - 25.0);
}

int main() {
    printf("=== ExecutionCore smoke test ===\n\n");

    uint64_t tsc_hz = calibrate_tsc_hz();
    if (tsc_hz == 0) {
        fprintf(stderr, "TSC calibration failed\n");
        return 1;
    }
    printf("TSC frequency: %.4f GHz\n", tsc_hz / 1e9);

    printf("\n--- functional tests ---\n");
    test_init_state();
    printf("init_state                       ok\n");
    test_no_trade_without_permission();
    printf("no_trade_without_permission      ok\n");
    test_entry_when_permitted();
    printf("entry_when_permitted             ok\n");
    test_no_double_entry();
    printf("no_double_entry                  ok\n");
    test_exit_on_tp();
    printf("exit_on_tp                       ok\n");
    test_exit_on_sl();
    printf("exit_on_sl                       ok\n");
    test_permission_revoke_during_trade();
    printf("permission_revoke_during_trade   ok\n");

    measure_tick_latency(tsc_hz);

    printf("\n=== %s (%d failures) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
