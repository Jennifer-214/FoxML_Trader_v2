// bench_production_vs_sharded.cpp
//
// Real apples-to-apples latency benchmark of the legacy single-threaded hot
// path versus the per-core sharded hot path. Both run on the same synthetic
// tick stream after a warmup period. Measurements are in TSC cycles converted
// to nanoseconds via runtime calibration.
//
// LEGACY hot path measured = PortfolioController_Tick (which runs the buy
// gate, position exit gate, fill drain, slow-path bookkeeping inline).
//
// SHARDED hot path measured = ExecutionCore_Tick × N cores + EventLoop drain
// (the cost the controller core would pay if it were running everything on
// one thread). In production each ExecutionCore_Tick runs on its own pinned
// thread, so the wall-clock per-market-tick cost would be max(per_core_cost)
// not sum(per_core_cost) — but for an honest comparison we report the total
// per-tick work as well as the per-core cost.
//
// Honest about what this DOESN'T measure:
//   - Cross-core cache coherence cost (single thread benchmark)
//   - SPSC ring contention (no real producer/consumer)
//   - Thread scheduling overhead
//   - The "fan in" cost on the controller side draining N cores' events
//
// What it DOES measure:
//   - The raw per-tick CPU cost of each architecture's hot path
//   - The scaling with position count (1, 4, 16)
//   - The seqlock parameter read overhead in the per-core path

#include "CoreFrameworks/ControllerEventLoop.hpp"
#include "CoreFrameworks/ExecutionCore.hpp"
#include "CoreFrameworks/PortfolioController.hpp"
#include "CoreFrameworks/Tick.hpp"
#include "DataStream/FauxFIX.hpp"
#include "DataStream/TradeLog.hpp"
#include "Strategies/StrategyParameters.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <vector>

using namespace tt;

//======================================================================================================
// TSC calibration + measurement helpers
//======================================================================================================
static inline uint64_t rdtsc_start() {
    uint32_t hi, lo;
    asm volatile("mfence\n\t"
                 "lfence\n\t"
                 "rdtsc\n\t"
                 : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_end() {
    uint32_t hi, lo;
    asm volatile("rdtscp\n\t"
                 "lfence\n\t"
                 : "=a"(lo), "=d"(hi)
                 :
                 : "rcx");
    return ((uint64_t)hi << 32) | lo;
}

static double calibrate_tsc_ghz() {
    uint64_t t0 = rdtsc_start();
    auto wall0 = std::chrono::high_resolution_clock::now();
    // Burn ~50ms of work
    volatile uint64_t x = 0;
    for (uint64_t i = 0; i < 50'000'000; ++i) x ^= i;
    (void)x;
    uint64_t t1 = rdtsc_end();
    auto wall1 = std::chrono::high_resolution_clock::now();
    double wall_ns = std::chrono::duration<double, std::nano>(wall1 - wall0).count();
    double cycles = (double)(t1 - t0);
    return cycles / wall_ns; // GHz
}

struct Stats {
    uint64_t min_cycles;
    uint64_t max_cycles;
    double avg_cycles;
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;
};

static Stats compute_stats(std::vector<uint64_t>& samples) {
    Stats s{};
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    s.min_cycles = samples.front();
    s.max_cycles = samples.back();
    uint64_t sum = 0;
    for (auto v : samples) sum += v;
    s.avg_cycles = (double)sum / samples.size();
    s.p50 = samples[samples.size() * 50 / 100];
    s.p95 = samples[samples.size() * 95 / 100];
    s.p99 = samples[samples.size() * 99 / 100];
    return s;
}

static void print_stats(const char* label, const Stats& s, double ghz) {
    auto cyc_to_ns = [&](double c) { return c / ghz; };
    printf("  %-30s  cycles: min=%6lu p50=%6lu p95=%6lu p99=%6lu max=%6lu avg=%7.1f\n",
           label,
           (unsigned long)s.min_cycles, (unsigned long)s.p50,
           (unsigned long)s.p95, (unsigned long)s.p99, (unsigned long)s.max_cycles,
           s.avg_cycles);
    printf("  %-30s     ns: min=%6.1f p50=%6.1f p95=%6.1f p99=%6.1f max=%6.1f avg=%7.1f\n",
           "",
           cyc_to_ns(s.min_cycles), cyc_to_ns(s.p50),
           cyc_to_ns(s.p95), cyc_to_ns(s.p99),
           cyc_to_ns(s.max_cycles), cyc_to_ns(s.avg_cycles));
}

//======================================================================================================
// Synthetic tick stream — sawtooth around a base price
//======================================================================================================
struct SyntheticTick {
    double price;
    double volume;
};

static std::vector<SyntheticTick> make_tick_stream(int n, double base_price) {
    std::vector<SyntheticTick> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        double phase = (double)(i % 200);
        double price = base_price + (phase < 100.0 ? phase * 2.0 : (200.0 - phase) * 2.0);
        v.push_back({price, 2.0});  // 2 BTC per trade, sane volume
    }
    return v;
}

//======================================================================================================
// LEGACY benchmark — full PortfolioController_Tick per market tick
//======================================================================================================
static Stats bench_legacy(const std::vector<SyntheticTick>& ticks, int max_positions, double ghz) {
    (void)ghz;
    // Build a full controller config (defaults are fine, override max_positions)
    ControllerConfig<64> cfg = ControllerConfig_Default<64>();
    cfg.max_positions = (uint32_t)max_positions;
    cfg.default_strategy = STRATEGY_SIMPLE_DIP;
    cfg.starting_balance = FPN_FromDouble<64>(100000.0);  // big balance so sizing always works
    cfg.warmup_ticks = 64;  // short warmup so we exit warmup quickly
    cfg.poll_interval = 64;
    cfg.entry_offset_pct = FPN_FromDouble<64>(0.0001);  // tight so BG fires often
    cfg.volume_multiplier = FPN_FromDouble<64>(0.1);
    cfg.slow_path_max_secs = 999999;

    PortfolioController<64> ctrl;
    ctrl.rolling_long = nullptr;
    PortfolioController_Init(&ctrl, cfg);

    OrderPool<64> pool;
    OrderPool_init(&pool, 64);

    // No real trade log on the bench; pass a stub
    TradeLog log;
    memset(&log, 0, sizeof(log));

    // Warmup: feed 1024 ticks so RollingStats fills up and we exit warmup
    for (int i = 0; i < 1024 && i < (int)ticks.size(); ++i) {
        FPN<64> p = FPN_FromDouble<64>(ticks[i].price);
        FPN<64> v = FPN_FromDouble<64>(ticks[i].volume);
        // Build the data stream the production engine uses
        DataStream<64> stream;
        stream.price = p;
        stream.volume = v;
        stream.price_d = ticks[i].price;
        stream.volume_d = ticks[i].volume;
        stream.is_buyer_maker = 0;
        // Match the production hot path: BuyGate inline, PositionExitGate, then PortfolioController_Tick
        if (ctrl.portfolio.active_bitmap)
            PositionExitGate(&ctrl.portfolio, p, &ctrl.exit_buf, ctrl.total_ticks);
        BuyGate(&ctrl.buy_conds, &stream, &pool);
        PortfolioController_Tick(&ctrl, &pool, p, v, &log, 0);
    }

    // Measurement phase
    std::vector<uint64_t> samples;
    samples.reserve(ticks.size());

    for (int i = 1024; i < (int)ticks.size(); ++i) {
        FPN<64> p = FPN_FromDouble<64>(ticks[i].price);
        FPN<64> vol = FPN_FromDouble<64>(ticks[i].volume);
        DataStream<64> stream;
        stream.price = p;
        stream.volume = vol;
        stream.price_d = ticks[i].price;
        stream.volume_d = ticks[i].volume;
        stream.is_buyer_maker = 0;

        uint64_t t0 = rdtsc_start();
        if (ctrl.portfolio.active_bitmap)
            PositionExitGate(&ctrl.portfolio, p, &ctrl.exit_buf, ctrl.total_ticks);
        BuyGate(&ctrl.buy_conds, &stream, &pool);
        PortfolioController_Tick(&ctrl, &pool, p, vol, &log, 0);
        uint64_t t1 = rdtsc_end();
        samples.push_back(t1 - t0);
    }

    if (ctrl.rolling_long) free(ctrl.rolling_long);
    return compute_stats(samples);
}

//======================================================================================================
// SHARDED benchmark — N execution cores fan-out + drain
//======================================================================================================
static Stats bench_sharded(const std::vector<SyntheticTick>& ticks, int num_cores, double ghz) {
    (void)ghz;
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(100000.0), FPN_FromDouble<64>(0.001));

    static SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tick_rings[16];
    static ExecutionCore<64> cores[16];

    for (int i = 0; i < num_cores; ++i) {
        SPSCRing_Init(&tick_rings[i]);
        ExecutionCore_Init(&cores[i], (uint16_t)i, &tick_rings[i]);
        EventLoopState_RegisterCore(&state, &cores[i],
            FPN_FromDouble<64>(0.0), FPN_FromDouble<64>(0.0), FPN_FromDouble<64>(0.0));
        EventLoopState_SetCoreStrategy(&state, i, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));
    }

    // Build tight gate parameters so BG fires roughly as often as legacy
    GateParameters<64> params;
    GateParameters_Init(&params);
    params.bg_price_threshold   = FPN_FromDouble<64>(60050.0);  // BG fires often on the sawtooth
    params.bg_volume_threshold  = FPN_FromDouble<64>(1.0);
    params.sg_take_profit_price = FPN_FromDouble<64>(60150.0);
    params.sg_stop_loss_price   = FPN_FromDouble<64>(59850.0);
    params.trade_size           = FPN_FromDouble<64>(0.01);
    params.strategy_id          = STRATEGY_SIMPLE_DIP;
    params.flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;

    for (int i = 0; i < num_cores; ++i) {
        ExecutionCore_SetParameters(&cores[i], params);
        ExecutionCore_SetPermission(&cores[i], 1);
    }

    // Warmup
    for (int i = 0; i < 1024 && i < (int)ticks.size(); ++i) {
        Tick<64> t;
        memset(&t, 0, sizeof(t));
        t.price = FPN_FromDouble<64>(ticks[i].price);
        t.volume = FPN_FromDouble<64>(ticks[i].volume);
        t.timestamp = (uint64_t)(i * 1000);
        for (int c = 0; c < num_cores; ++c) {
            ExecutionCore_Tick(&cores[c], t);
        }
        EventLoop_DrainEvents(&state);
    }

    // Measurement
    std::vector<uint64_t> samples;
    samples.reserve(ticks.size());

    for (int i = 1024; i < (int)ticks.size(); ++i) {
        Tick<64> t;
        memset(&t, 0, sizeof(t));
        t.price = FPN_FromDouble<64>(ticks[i].price);
        t.volume = FPN_FromDouble<64>(ticks[i].volume);
        t.timestamp = (uint64_t)(i * 1000);

        uint64_t t0 = rdtsc_start();
        for (int c = 0; c < num_cores; ++c) {
            ExecutionCore_Tick(&cores[c], t);
        }
        EventLoop_DrainEvents(&state);
        uint64_t t1 = rdtsc_end();
        samples.push_back(t1 - t0);
    }

    return compute_stats(samples);
}

//======================================================================================================
// SHARDED PER CORE — single core hot path only (no fan out)
//======================================================================================================
static Stats bench_sharded_one_core(const std::vector<SyntheticTick>& ticks, double ghz) {
    (void)ghz;
    OrderManagerState<64> oms;
    EventLoopState<64> state;
    EventLoopState_InitLegacy(&state, &oms, FPN_FromDouble<64>(100000.0), FPN_FromDouble<64>(0.001));

    static SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> tr;
    SPSCRing_Init(&tr);
    static ExecutionCore<64> core;
    ExecutionCore_Init(&core, 0, &tr);
    EventLoopState_RegisterCore(&state, &core,
        FPN_FromDouble<64>(0.0), FPN_FromDouble<64>(0.0), FPN_FromDouble<64>(0.0));
    EventLoopState_SetCoreStrategy(&state, 0, STRATEGY_SIMPLE_DIP, FPN_FromDouble<64>(1000.0));

    GateParameters<64> params;
    GateParameters_Init(&params);
    params.bg_price_threshold   = FPN_FromDouble<64>(60050.0);
    params.bg_volume_threshold  = FPN_FromDouble<64>(1.0);
    params.sg_take_profit_price = FPN_FromDouble<64>(60150.0);
    params.sg_stop_loss_price   = FPN_FromDouble<64>(59850.0);
    params.trade_size           = FPN_FromDouble<64>(0.01);
    params.strategy_id          = STRATEGY_SIMPLE_DIP;
    params.flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    ExecutionCore_SetParameters(&core, params);
    ExecutionCore_SetPermission(&core, 1);

    // Warmup
    for (int i = 0; i < 1024 && i < (int)ticks.size(); ++i) {
        Tick<64> t;
        memset(&t, 0, sizeof(t));
        t.price = FPN_FromDouble<64>(ticks[i].price);
        t.volume = FPN_FromDouble<64>(ticks[i].volume);
        ExecutionCore_Tick(&core, t);
    }

    std::vector<uint64_t> samples;
    samples.reserve(ticks.size());

    for (int i = 1024; i < (int)ticks.size(); ++i) {
        Tick<64> t;
        memset(&t, 0, sizeof(t));
        t.price = FPN_FromDouble<64>(ticks[i].price);
        t.volume = FPN_FromDouble<64>(ticks[i].volume);

        uint64_t t0 = rdtsc_start();
        ExecutionCore_Tick(&core, t);
        uint64_t t1 = rdtsc_end();
        samples.push_back(t1 - t0);
    }

    return compute_stats(samples);
}

//======================================================================================================
// main
//======================================================================================================
int main() {
    double ghz = calibrate_tsc_ghz();
    printf("=== Production vs Sharded latency benchmark ===\n");
    printf("TSC calibrated at %.4f GHz\n", ghz);
    printf("Same synthetic tick stream (sawtooth around $60k, 200k ticks)\n");
    printf("First 1024 ticks discarded as warmup\n\n");

    auto ticks = make_tick_stream(200000, 60000.0);

    printf("LEGACY: BuyGate + PositionExitGate + PortfolioController_Tick (full live engine hot path)\n");
    for (int npos : {1, 4, 16}) {
        printf("\n--- max_positions = %d ---\n", npos);
        Stats s = bench_legacy(ticks, npos, ghz);
        char label[64];
        snprintf(label, sizeof(label), "legacy hot path (%d pos)", npos);
        print_stats(label, s, ghz);
    }

    printf("\nSHARDED: ExecutionCore_Tick × N + EventLoop_DrainEvents (per-tick, single-thread)\n");
    {
        printf("\n--- 1 execution core (single per-core path, no fan out) ---\n");
        Stats s = bench_sharded_one_core(ticks, ghz);
        print_stats("sharded 1 core (single tick)", s, ghz);
    }
    for (int n : {1, 4, 16}) {
        printf("\n--- %d execution cores (fan out cost) ---\n", n);
        Stats s = bench_sharded(ticks, n, ghz);
        char label[64];
        snprintf(label, sizeof(label), "sharded fan out (%d cores)", n);
        print_stats(label, s, ghz);
    }

    printf("\n=== Notes ===\n");
    printf("- Sharded fan-out is the SUM of per-core costs run sequentially on one thread.\n");
    printf("  In production each core runs on its own pinned thread, so the wall-clock\n");
    printf("  per-market-tick cost would be ~max(per_core_cost) ~= 'sharded 1 core'.\n");
    printf("- Legacy hot path includes the full PortfolioController_Tick overhead\n");
    printf("  (RollingStats sample on slow path, regime, strategy dispatch, fill drain).\n");
    printf("- rdtsc + lfence/mfence has a floor of ~25-30ns on this CPU. Subtract that\n");
    printf("  from min/p50 to get the actual hot-path work cost.\n");
    return 0;
}
