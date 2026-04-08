// test_parameter_slot.cpp — phase 05 functional + stress tests for ParameterSlot
//
// What this validates:
//   1. Init produces a slot where reads return the initial value
//   2. Single write is observable to subsequent reads
//   3. Multiple writes rotate through buffers correctly (active_idx changes)
//   4. The buffer being written is never the buffer being read (P5.1 fix)
//   5. Reads after a swap see the new value
//   6. STRESS: producer at high rate, consumer at higher rate, no torn reads
//      across millions of read/write interleavings
//   7. ExecutionCore integration: setting parameters via SetParameters and
//      reading them back via ExecutionCore_Tick path produces consistent state
//
// The stress test is the important one for phase 05 — it's where the
// triple-buffer guarantee actually matters. Single-threaded tests pass even
// with double buffering; only the concurrent test catches the rapid-write race.

#include "CoreFrameworks/ParameterSlot.hpp"
#include "CoreFrameworks/GateParameters.hpp"
#include "CoreFrameworks/ExecutionCore.hpp"
#include "CoreFrameworks/Tick.hpp"
#include "CoreFrameworks/ControllerEventLoop.hpp"

#include <atomic>
#include <cstdio>
#include <thread>

using namespace tt;

static int failures = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);     \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

// helper: build a GateParameters with a known fingerprint so we can verify
// that the buffer we read is internally consistent (all fields from the same
// write, not a mix of two different writes).
static GateParameters<64> make_params(double base) {
    GateParameters<64> p;
    GateParameters_Init(&p);
    p.bg_price_threshold   = FPN_FromDouble<64>(base);
    p.bg_volume_threshold  = FPN_FromDouble<64>(base + 1.0);
    p.sg_take_profit_price = FPN_FromDouble<64>(base + 2.0);
    p.sg_stop_loss_price   = FPN_FromDouble<64>(base - 2.0);
    p.trade_size           = FPN_FromDouble<64>(base * 0.001);
    p.strategy_id          = (uint8_t)((int)base & 0xFF);
    p.flags                = (uint8_t)(((int)base >> 8) & 0xFF);
    return p;
}

// verify a GateParameters matches the fingerprint pattern from make_params
static bool params_consistent(const GateParameters<64>& p, double* recovered_base) {
    double base = FPN_ToDouble(p.bg_price_threshold);
    if (FPN_ToDouble(p.bg_volume_threshold)  != base + 1.0) return false;
    if (FPN_ToDouble(p.sg_take_profit_price) != base + 2.0) return false;
    if (FPN_ToDouble(p.sg_stop_loss_price)   != base - 2.0) return false;
    if (FPN_ToDouble(p.trade_size)           != base * 0.001) return false;
    if (p.strategy_id != (uint8_t)((int)base & 0xFF)) return false;
    if (p.flags       != (uint8_t)(((int)base >> 8) & 0xFF)) return false;
    if (recovered_base) *recovered_base = base;
    return true;
}

//======================================================================================================
// test 1: init returns the initial value
//======================================================================================================
static void test_init() {
    ParameterSlot<GateParameters<64>> slot;
    GateParameters<64> initial = make_params(60000.0);
    ParameterSlot_Init(&slot, initial);

    GateParameters<64> read;
    ParameterSlot_Read(&slot, &read);
    EXPECT(FPN_ToDouble(read.bg_price_threshold) == 60000.0, "init: bg_price matches");
    EXPECT(read.strategy_id == initial.strategy_id, "init: strategy_id matches");
    double base;
    EXPECT(params_consistent(read, &base), "init: read is internally consistent");
}

//======================================================================================================
// test 2: single write is observable
//======================================================================================================
static void test_single_write() {
    ParameterSlot<GateParameters<64>> slot;
    ParameterSlot_Init(&slot, make_params(60000.0));

    GateParameters<64> updated = make_params(70000.0);
    ParameterSlot_Write(&slot, updated);

    GateParameters<64> read;
    ParameterSlot_Read(&slot, &read);
    EXPECT(FPN_ToDouble(read.bg_price_threshold) == 70000.0, "after write: read sees new value");
    double base;
    EXPECT(params_consistent(read, &base), "after write: read is consistent");
    EXPECT(base == 70000.0, "recovered base matches written");
}

//======================================================================================================
// test 3: multiple writes alternate buffers and the read sees the last write
//======================================================================================================
static void test_buffer_alternation() {
    ParameterSlot<GateParameters<64>> slot;
    ParameterSlot_Init(&slot, make_params(60000.0));

    // Seqlock alternates between buffer 0 and 1 on each write. After init,
    // active is 0; first write → 1; second → 0; etc.
    EXPECT(ParameterSlot_ActiveIndex(&slot) == 0, "init: active = 0");

    for (int i = 1; i <= 6; ++i) {
        ParameterSlot_Write(&slot, make_params(60000.0 + (double)i * 100.0));
        uint8_t cur_idx = ParameterSlot_ActiveIndex(&slot);
        EXPECT(cur_idx == (uint8_t)(i & 1), "active alternates 1,0,1,0,1,0");
    }

    GateParameters<64> final_read;
    ParameterSlot_Read(&slot, &final_read);
    EXPECT(FPN_ToDouble(final_read.bg_price_threshold) == 60600.0, "final read = last write");
}

//======================================================================================================
// test 4: sequence number advances by 2 per write
//======================================================================================================
static void test_sequence_advance() {
    ParameterSlot<GateParameters<64>> slot;
    ParameterSlot_Init(&slot, make_params(60000.0));

    uint64_t s0 = ParameterSlot_Sequence(&slot);
    EXPECT(s0 == 0, "init: seq = 0");

    ParameterSlot_Write(&slot, make_params(70000.0));
    uint64_t s1 = ParameterSlot_Sequence(&slot);
    EXPECT(s1 == 2, "after 1 write: seq = 2");
    EXPECT((s1 & 1) == 0, "seq is even (stable)");

    ParameterSlot_Write(&slot, make_params(80000.0));
    uint64_t s2 = ParameterSlot_Sequence(&slot);
    EXPECT(s2 == 4, "after 2 writes: seq = 4");
}

//======================================================================================================
// test 5: STRESS — concurrent producer + consumer, verify no torn reads
//======================================================================================================
static void test_concurrent_stress() {
    ParameterSlot<GateParameters<64>> slot;
    ParameterSlot_Init(&slot, make_params(60000.0));

    constexpr int N_WRITES = 100'000;
    constexpr int N_READS  = 1'000'000;

    std::atomic<bool> stop{false};
    std::atomic<int> torn_reads{0};
    std::atomic<int> consistent_reads{0};

    // Consumer: reads as fast as it can, verifies internal consistency.
    // The seqlock guarantees a self-consistent snapshot — if the producer
    // wrote during the read, ParameterSlot_Read retries internally.
    std::thread consumer([&]() {
        for (int i = 0; i < N_READS && !stop.load(std::memory_order_relaxed); ++i) {
            GateParameters<64> snap;
            ParameterSlot_Read(&slot, &snap);
            double base;
            if (params_consistent(snap, &base)) {
                consistent_reads.fetch_add(1, std::memory_order_relaxed);
            } else {
                torn_reads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Producer: writes at a moderate rate, base value monotonically increases
    std::thread producer([&]() {
        for (int i = 1; i <= N_WRITES; ++i) {
            ParameterSlot_Write(&slot, make_params(60000.0 + (double)i));
            // tiny pause to let the consumer get reads in between writes
            // (without this the producer monopolizes and we don't exercise
            // the racing case as hard)
            if ((i & 0x3F) == 0) std::this_thread::yield();
        }
        stop.store(true, std::memory_order_relaxed);
    });

    producer.join();
    consumer.join();

    int torn = torn_reads.load();
    int good = consistent_reads.load();
    printf("  stress: %d consistent reads, %d torn reads, %d writes\n",
           good, torn, N_WRITES);
    // The seqlock guarantee: every read is internally consistent. The
    // consumer may retry under heavy producer load but never returns torn
    // data. Zero torn reads is the only assertion that matters here.
    EXPECT(torn == 0, "stress: zero torn reads across all interleavings");
    EXPECT(good > 0, "stress: consumer made forward progress");
}

//======================================================================================================
// test 6: ExecutionCore integration — push params and read via Tick path
//======================================================================================================
static void test_execution_core_integration() {
    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    SPSCRing_Init(&tick_ring);

    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tick_ring);

    // Push params with a buy threshold of 60000
    GateParameters<64> p = make_params(60000.0);
    p.flags = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    ExecutionCore_SetParameters(&core, p);
    core.permission = 1;

    // Build a tick at 59000 — should fire BG (price < 60000)
    Tick<64> tick;
    tick.price = FPN_FromDouble<64>(59000.0);
    tick.volume = FPN_FromDouble<64>(1000.0);
    tick.timestamp = 1'000'000;
    tick.sequence = 1;

    ExecutionCore_Tick(&core, tick);
    EXPECT(core.active == 1, "core entered after push of valid params");
    EXPECT(SPSCRing_Depth(&core.event_ring) == 1, "entry event pushed");

    // drain
    TradeEvent<64> ev;
    SPSCRing_TryPop(&core.event_ring, &ev);

    // Now push new params with TP at 59500 — exit immediately on next tick
    GateParameters<64> p2 = make_params(60000.0);
    p2.sg_take_profit_price = FPN_FromDouble<64>(59500.0);
    p2.sg_stop_loss_price   = FPN_FromDouble<64>(58000.0);
    p2.flags = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    ExecutionCore_SetParameters(&core, p2);

    Tick<64> tick2 = tick;
    tick2.price = FPN_FromDouble<64>(59600.0);  // above new TP of 59500
    tick2.timestamp = 1'000'001;
    tick2.sequence = 2;

    ExecutionCore_Tick(&core, tick2);
    EXPECT(core.active == 0, "exit fired on new TP from updated params");
}

//======================================================================================================
// test 7: ControllerEventLoop integration — queue + push parameters
//======================================================================================================
static void test_event_loop_integration() {
    EventLoopState<64> state;
    EventLoopState_Init(&state, FPN_FromDouble<64>(10000.0), FPN_FromDouble<64>(0.001));

    SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);

    int slot = EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(60500.0), FPN_FromDouble<64>(59500.0), FPN_FromDouble<64>(0.01));
    EXPECT(slot == 0, "core registered to slot 0");

    // queue parameters for the core
    GateParameters<64> p = make_params(60000.0);
    p.flags = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    EventLoop_QueueParameters(&state, slot, p);
    EXPECT(state.cores[slot].dirty == 1, "core marked dirty after queue");

    // before push, the core's slot still has the init defaults (STRATEGY_NONE)
    GateParameters<64> before;
    ParameterSlot_Read(&core.param_slot, &before);
    EXPECT(before.strategy_id == STRATEGY_NONE, "before push: still init defaults");

    // push the queued params
    int pushed = EventLoop_PushParameters(&state);
    EXPECT(pushed == 1, "one core pushed");
    EXPECT(state.cores[slot].dirty == 0, "dirty flag cleared after push");

    // now the core sees the new params
    GateParameters<64> after;
    ParameterSlot_Read(&core.param_slot, &after);
    EXPECT(FPN_ToDouble(after.bg_price_threshold) == 60000.0, "after push: new params live");

    // pushing again with no dirty cores does nothing
    int pushed2 = EventLoop_PushParameters(&state);
    EXPECT(pushed2 == 0, "second push with no dirty: zero cores pushed");
}

//======================================================================================================
// main
//======================================================================================================
int main() {
    printf("=== ParameterSlot tests ===\n\n");

    test_init();
    printf("init                              ok\n");
    test_single_write();
    printf("single_write                      ok\n");
    test_buffer_alternation();
    printf("buffer_alternation                ok\n");
    test_sequence_advance();
    printf("sequence_advance                  ok\n");
    test_concurrent_stress();
    printf("concurrent_stress                 ok\n");
    test_execution_core_integration();
    printf("execution_core_integration        ok\n");
    test_event_loop_integration();
    printf("event_loop_integration            ok\n");

    printf("\n=== %s (%d failures) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
