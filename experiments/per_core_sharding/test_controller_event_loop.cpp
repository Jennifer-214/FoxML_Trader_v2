// test_controller_event_loop.cpp — phase 04 functional tests
//
// Validates the controller event loop end to end:
//   1. Init produces clean state (zero balance impact, no cores registered)
//   2. RegisterCore assigns slot == core_id
//   3. Single entry event opens the right portfolio slot
//   4. Single exit event closes the slot and updates balance with net P&L
//   5. Entry+exit pair across multiple cores produces correct balance
//   6. Drain loop processes events in arrival-per-core order
//   7. Per-core drain cap prevents one core from monopolizing a pass
//   8. Round-robin doesn't starve any core under burst load
//   9. RunController loop exits cleanly on shutdown flag
//
// Hot-path latency NOT measured here — that's phase 12 / phase 03's job.
// This test is purely about correctness of the event-to-state pipeline.

#include "CoreFrameworks/ControllerEventLoop.hpp"
#include "CoreFrameworks/ExecutionCore.hpp"
#include "CoreFrameworks/Tick.hpp"
#include "CoreFrameworks/TradeEvent.hpp"

#include <cstdio>
#include <thread>
#include <chrono>

using namespace tt;

static int failures = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);     \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

// Helper: build a TradeEvent for a given core, type, price.
static TradeEvent<64> make_event(uint16_t core_id, uint8_t type, double price, uint64_t timestamp) {
    TradeEvent<64> ev{};
    ev.price = FPN_FromDouble<64>(price);
    ev.timestamp = timestamp;
    ev.core_id = core_id;
    ev.type = type;
    return ev;
}

// Helper: push a TradeEvent into a core's event ring.
static bool push_event(ExecutionCore<64>* core, const TradeEvent<64>& ev) {
    return SPSCRing_TryPush(&core->event_ring, ev);
}

// Helper: build an ExecutionCore<64> initialized for testing. Tick ring is
// shared from a stack-allocated SPSCRing the caller owns.
static void init_test_core(ExecutionCore<64>* core,
                           SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE>* tick_ring,
                           uint16_t core_id) {
    ExecutionCore_Init(core, core_id, tick_ring);
}

//======================================================================================================
// test 1: init produces clean state
//======================================================================================================
static void test_init() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    EXPECT(state.registered_count == 0, "init: no cores registered");
    EXPECT(FPN_ToDouble(state.oms->balance) == 10000.0, "init: balance == starting");
    EXPECT(FPN_ToDouble(state.oms->realized_pnl) == 0.0, "init: realized_pnl == 0");
    EXPECT(state.total_events_processed == 0, "init: events == 0");
    EXPECT(state.oms->portfolio.active_bitmap == 0, "init: portfolio empty");
}

//======================================================================================================
// test 2: register core assigns slot == core_id
//======================================================================================================
static void test_register_core() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);

    ExecutionCore<64> core_a, core_b, core_c;
    init_test_core(&core_a, &tick_ring, 99);  // intentionally bogus initial core_id
    init_test_core(&core_b, &tick_ring, 99);
    init_test_core(&core_c, &tick_ring, 99);

    int slot_a = EventLoopState_RegisterCore(&state, &core_a,
        FPN_FromDouble<64>(60100.0), FPN_FromDouble<64>(59900.0), FPN_FromDouble<64>(0.01));
    int slot_b = EventLoopState_RegisterCore(&state, &core_b,
        FPN_FromDouble<64>(60200.0), FPN_FromDouble<64>(59800.0), FPN_FromDouble<64>(0.02));
    int slot_c = EventLoopState_RegisterCore(&state, &core_c,
        FPN_FromDouble<64>(60300.0), FPN_FromDouble<64>(59700.0), FPN_FromDouble<64>(0.03));

    EXPECT(slot_a == 0, "first registered → slot 0");
    EXPECT(slot_b == 1, "second registered → slot 1");
    EXPECT(slot_c == 2, "third registered → slot 2");
    EXPECT(core_a.core_id == 0, "core_a.core_id rewritten to slot");
    EXPECT(core_b.core_id == 1, "core_b.core_id rewritten to slot");
    EXPECT(core_c.core_id == 2, "core_c.core_id rewritten to slot");
    EXPECT(state.registered_count == 3, "three cores registered");
    EXPECT(state.cores[0].core == &core_a, "ctx[0].core points to core_a");
    EXPECT(state.cores[1].core == &core_b, "ctx[1].core points to core_b");
    EXPECT(state.cores[2].core == &core_c, "ctx[2].core points to core_c");
    EXPECT(FPN_ToDouble(state.cores[0].intended_qty) == 0.01, "ctx[0] qty captured");
    EXPECT(FPN_ToDouble(state.cores[1].intended_tp)  == 60200.0, "ctx[1] tp captured");
}

//======================================================================================================
// test 3: single entry event opens the slot
//======================================================================================================
static void test_single_entry() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);

    ExecutionCore<64> core;
    init_test_core(&core, &tick_ring, 0);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60100.0), FPN_FromDouble<64>(59900.0), FPN_FromDouble<64>(0.01));

    TradeEvent<64> entry = make_event((uint16_t)slot, TRADE_EVENT_ENTRY, 60000.0, 1'000'000);
    EventLoop_OnEvent(&state, entry);

    EXPECT(Portfolio_SlotActive(&state.oms->portfolio, slot) == 1, "slot active after entry");
    EXPECT(FPN_ToDouble(state.oms->portfolio.positions[slot].entry_price) == 60000.0, "entry_price recorded");
    EXPECT(FPN_ToDouble(state.oms->portfolio.positions[slot].take_profit_price) == 60100.0, "tp from intended");
    EXPECT(FPN_ToDouble(state.oms->portfolio.positions[slot].stop_loss_price) == 59900.0, "sl from intended");
    EXPECT(FPN_ToDouble(state.oms->portfolio.positions[slot].quantity) == 0.01, "qty from intended");
    EXPECT(state.cores[slot].entries_processed == 1, "core entries++");
    EXPECT(state.total_entries == 1, "global entries++");
    EXPECT(state.total_events_processed == 1, "events++");

    // entry fee = 60000 * 0.01 * 0.001 = 0.6
    double recorded_fee = FPN_ToDouble(state.oms->portfolio.positions[slot].entry_fee);
    EXPECT(recorded_fee > 0.59 && recorded_fee < 0.61, "entry fee = price * qty * rate");
}

//======================================================================================================
// test 4: single exit event closes the slot and updates balance
//======================================================================================================
static void test_single_exit() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);

    ExecutionCore<64> core;
    init_test_core(&core, &tick_ring, 0);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60100.0), FPN_FromDouble<64>(59900.0), FPN_FromDouble<64>(0.01));

    // entry first
    EventLoop_OnEvent(&state, make_event((uint16_t)slot, TRADE_EVENT_ENTRY, 60000.0, 1'000'000));
    // exit at 60100 (TP hit)
    EventLoop_OnEvent(&state, make_event((uint16_t)slot, TRADE_EVENT_EXIT,  60100.0, 1'000'001));

    EXPECT(Portfolio_SlotActive(&state.oms->portfolio, slot) == 0, "slot inactive after exit");
    EXPECT(state.cores[slot].exits_processed == 1, "core exits++");
    EXPECT(state.total_exits == 1, "global exits++");
    EXPECT(state.total_events_processed == 2, "events == 2");

    // gross = (60100 - 60000) * 0.01 = 1.00
    // entry_fee = 60000 * 0.01 * 0.001 = 0.60
    // exit_fee  = 60100 * 0.01 * 0.001 = 0.601
    // net = 1.00 - 1.201 = -0.201   (small loss due to fees > gross profit)
    double balance = FPN_ToDouble(state.oms->balance);
    double pnl = FPN_ToDouble(state.oms->realized_pnl);
    double expected_net = 1.00 - 0.60 - 0.601;
    EXPECT(pnl < expected_net + 0.01 && pnl > expected_net - 0.01, "net P&L matches gross - fees");
    EXPECT(balance > 9999.5 && balance < 10000.0, "balance updated by net");
}

//======================================================================================================
// test 5: profitable trade nets positive P&L
//======================================================================================================
static void test_profitable_trade() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);

    ExecutionCore<64> core;
    init_test_core(&core, &tick_ring, 0);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(61000.0), FPN_FromDouble<64>(59000.0), FPN_FromDouble<64>(0.5));

    // big move: 60000 → 61000, qty 0.5 → gross = 500
    // entry_fee = 60000 * 0.5 * 0.001 = 30
    // exit_fee  = 61000 * 0.5 * 0.001 = 30.5
    // net = 500 - 60.5 = 439.5
    EventLoop_OnEvent(&state, make_event((uint16_t)slot, TRADE_EVENT_ENTRY, 60000.0, 1'000'000));
    EventLoop_OnEvent(&state, make_event((uint16_t)slot, TRADE_EVENT_EXIT,  61000.0, 1'000'100));

    double pnl = FPN_ToDouble(state.oms->realized_pnl);
    EXPECT(pnl > 439.0 && pnl < 440.0, "profitable trade nets ~439.5");
    EXPECT(FPN_ToDouble(state.oms->balance) > 10439.0, "balance grew");
}

//======================================================================================================
// test 6: entry+exit across multiple cores produces correct balance
//======================================================================================================
static void test_multi_core_pairs() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr_a, tr_b, tr_c;
    SPSCRing_Init(&tr_a); SPSCRing_Init(&tr_b); SPSCRing_Init(&tr_c);

    ExecutionCore<64> ca, cb, cc;
    init_test_core(&ca, &tr_a, 0);
    init_test_core(&cb, &tr_b, 0);
    init_test_core(&cc, &tr_c, 0);

    int sa = EventLoopState_RegisterCore(&state, &ca, FPN_FromDouble<64>(61000.0), FPN_FromDouble<64>(59000.0), FPN_FromDouble<64>(0.1));
    int sb = EventLoopState_RegisterCore(&state, &cb, FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.2));
    int sc = EventLoopState_RegisterCore(&state, &cc, FPN_FromDouble<64>(60800.0), FPN_FromDouble<64>(59200.0), FPN_FromDouble<64>(0.05));

    // 3 entries followed by 3 exits, each profitable
    EventLoop_OnEvent(&state, make_event((uint16_t)sa, TRADE_EVENT_ENTRY, 60000.0, 100));
    EventLoop_OnEvent(&state, make_event((uint16_t)sb, TRADE_EVENT_ENTRY, 60100.0, 101));
    EventLoop_OnEvent(&state, make_event((uint16_t)sc, TRADE_EVENT_ENTRY, 59900.0, 102));

    EXPECT(state.oms->portfolio.active_bitmap == 0b111, "three slots active");
    EXPECT(state.total_entries == 3, "3 entries");

    EventLoop_OnEvent(&state, make_event((uint16_t)sa, TRADE_EVENT_EXIT, 61000.0, 200));
    EventLoop_OnEvent(&state, make_event((uint16_t)sb, TRADE_EVENT_EXIT, 60500.0, 201));
    EventLoop_OnEvent(&state, make_event((uint16_t)sc, TRADE_EVENT_EXIT, 60800.0, 202));

    EXPECT(state.oms->portfolio.active_bitmap == 0, "all slots closed");
    EXPECT(state.total_exits == 3, "3 exits");
    EXPECT(state.total_events_processed == 6, "6 events processed");
    EXPECT(FPN_ToDouble(state.oms->realized_pnl) > 0.0, "net P&L positive");

    // each core has 1 entry + 1 exit
    EXPECT(state.cores[sa].entries_processed == 1 && state.cores[sa].exits_processed == 1, "core a: 1+1");
    EXPECT(state.cores[sb].entries_processed == 1 && state.cores[sb].exits_processed == 1, "core b: 1+1");
    EXPECT(state.cores[sc].entries_processed == 1 && state.cores[sc].exits_processed == 1, "core c: 1+1");
}

//======================================================================================================
// test 7: drain loop processes events from event rings (round-robin)
//======================================================================================================
static void test_drain_loop() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr_a, tr_b;
    SPSCRing_Init(&tr_a); SPSCRing_Init(&tr_b);

    ExecutionCore<64> ca, cb;
    init_test_core(&ca, &tr_a, 0);
    init_test_core(&cb, &tr_b, 0);

    int sa = EventLoopState_RegisterCore(&state, &ca,
        FPN_FromDouble<64>(61000.0), FPN_FromDouble<64>(59000.0), FPN_FromDouble<64>(0.1));
    int sb = EventLoopState_RegisterCore(&state, &cb,
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.1));

    // push 2 events into core_a, 3 into core_b
    EXPECT(push_event(&ca, make_event((uint16_t)sa, TRADE_EVENT_ENTRY, 60000.0, 100)), "push a entry");
    EXPECT(push_event(&ca, make_event((uint16_t)sa, TRADE_EVENT_EXIT,  61000.0, 200)), "push a exit");
    EXPECT(push_event(&cb, make_event((uint16_t)sb, TRADE_EVENT_ENTRY, 60100.0, 101)), "push b entry");
    EXPECT(push_event(&cb, make_event((uint16_t)sb, TRADE_EVENT_EXIT,  60500.0, 201)), "push b exit");
    EXPECT(push_event(&cb, make_event((uint16_t)sb, TRADE_EVENT_ENTRY, 60200.0, 202)), "push b entry 2");

    int drained = EventLoop_DrainEvents(&state);
    EXPECT(drained == 5, "drained all 5 events in one pass");
    EXPECT(state.total_entries == 3, "3 entries");
    EXPECT(state.total_exits  == 2, "2 exits");
    EXPECT(state.oms->portfolio.active_bitmap == (1 << sb), "only core b's second entry left active");
}

//======================================================================================================
// test 8: per-core drain cap prevents one core from monopolizing
//======================================================================================================
static void test_drain_cap() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr_a, tr_b;
    SPSCRing_Init(&tr_a); SPSCRing_Init(&tr_b);

    ExecutionCore<64> ca, cb;
    init_test_core(&ca, &tr_a, 0);
    init_test_core(&cb, &tr_b, 0);

    int sa = EventLoopState_RegisterCore(&state, &ca,
        FPN_FromDouble<64>(61000.0), FPN_FromDouble<64>(59000.0), FPN_FromDouble<64>(0.001));
    int sb = EventLoopState_RegisterCore(&state, &cb,
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.001));

    // pile core_a with 100 events. core_b with just 1.
    // a single drain pass should pull MAX_EVENTS_PER_DRAIN_PER_CORE from a
    // (16 by default), leaving 84 in a's ring, AND drain b's 1 event in the
    // same pass — the cap is per-core, not global.
    for (int i = 0; i < 100; ++i) {
        // alternate entry/exit so portfolio state stays consistent
        uint8_t type = (i % 2 == 0) ? TRADE_EVENT_ENTRY : TRADE_EVENT_EXIT;
        push_event(&ca, make_event((uint16_t)sa, type, 60000.0 + (double)(i % 10), 1000 + i));
    }
    push_event(&cb, make_event((uint16_t)sb, TRADE_EVENT_ENTRY, 60050.0, 9999));

    int drained = EventLoop_DrainEvents(&state);
    // Expect: 16 from core_a + 1 from core_b = 17
    EXPECT(drained == MAX_EVENTS_PER_DRAIN_PER_CORE + 1, "drained cap_a + all_b");
    EXPECT(SPSCRing_Depth(&ca.event_ring) == 100 - MAX_EVENTS_PER_DRAIN_PER_CORE,
           "core_a still has remaining events");
    EXPECT(SPSCRing_Depth(&cb.event_ring) == 0, "core_b drained completely");
}

//======================================================================================================
// test 9: RunController exits cleanly on shutdown flag
//======================================================================================================
static void test_run_controller_shutdown() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);

    ExecutionCore<64> core;
    init_test_core(&core, &tr, 0);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(61000.0), FPN_FromDouble<64>(59000.0), FPN_FromDouble<64>(0.01));
    (void)slot;

    volatile int shutdown = 0;

    std::thread worker([&]() {
        EventLoop_RunController(&state, &shutdown);
    });

    // Push some events, give the controller a chance to process them
    push_event(&core, make_event(0, TRADE_EVENT_ENTRY, 60000.0, 100));
    push_event(&core, make_event(0, TRADE_EVENT_EXIT,  60500.0, 200));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    shutdown = 1;
    worker.join();

    EXPECT(state.total_entries == 1, "controller processed entry");
    EXPECT(state.total_exits == 1, "controller processed exit");
    EXPECT(state.total_events_processed == 2, "2 events total");
}

//======================================================================================================
// test 10: invalid event types are silently ignored
//======================================================================================================
static void test_invalid_event_type() {
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    init_test_core(&core, &tr, 0);
    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(61000.0), FPN_FromDouble<64>(59000.0), FPN_FromDouble<64>(0.01));

    // type=0 (no bits set)
    EventLoop_OnEvent(&state, make_event((uint16_t)slot, 0, 60000.0, 100));
    // type = ENTRY | EXIT (impossible, mutually exclusive)
    EventLoop_OnEvent(&state, make_event((uint16_t)slot, TRADE_EVENT_ENTRY | TRADE_EVENT_EXIT, 60000.0, 101));
    // out-of-range core_id
    EventLoop_OnEvent(&state, make_event(99, TRADE_EVENT_ENTRY, 60000.0, 102));

    EXPECT(state.total_events_processed == 0, "all 3 invalid events ignored");
    EXPECT(state.oms->portfolio.active_bitmap == 0, "no slots opened");
}

//======================================================================================================
// main
//======================================================================================================
int main() {
    printf("=== ControllerEventLoop functional tests ===\n\n");

    test_init();
    printf("init                              ok\n");
    test_register_core();
    printf("register_core                     ok\n");
    test_single_entry();
    printf("single_entry                      ok\n");
    test_single_exit();
    printf("single_exit                       ok\n");
    test_profitable_trade();
    printf("profitable_trade                  ok\n");
    test_multi_core_pairs();
    printf("multi_core_pairs                  ok\n");
    test_drain_loop();
    printf("drain_loop                        ok\n");
    test_drain_cap();
    printf("drain_cap                         ok\n");
    test_run_controller_shutdown();
    printf("run_controller_shutdown           ok\n");
    test_invalid_event_type();
    printf("invalid_event_type                ok\n");

    printf("\n=== %s (%d failures) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
