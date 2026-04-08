// bench_batch_floor.cpp — bracket rdtsc ONCE around N iterations of
// ExecutionCore_Tick, divide. amortizes the ~27ns rdtsc cost across the whole
// batch so the per-tick number is the actual steady-state work, not work +
// measurement Heisenberg. answers "how low can latency go" honestly.
//
// also prints the per-call bracketed number as a sanity check vs scenario C
// of bench_hot_path.cpp, so we can see the rdtsc tax directly.

#include "../../CoreFrameworks/ExecutionCore.hpp"
#include "../../CoreFrameworks/GateParameters.hpp"
#include "../../CoreFrameworks/SPSCRing.hpp"
#include "../../CoreFrameworks/Tick.hpp"
#include "../../FixedPoint/FixedPointN.hpp"
#include "../../Strategies/StrategyInterface.hpp"
#include "common/measurement.hpp"
#include "common/rdtsc.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace tt;
using namespace experiment;

// 256 ticks * 64B = 16KB — well inside the 32-48KB L1D on Ice Lake.
// Without this shrink, the bench was paying L2/L3 fetch on every tick.
constexpr int  TICK_STREAM_LEN  = 256;
constexpr size_t TICK_STREAM_MASK = TICK_STREAM_LEN - 1;
constexpr int  WARMUP_N         = 100'000;
constexpr int  PER_CALL_SAMPLES = 200'000;
constexpr int  BATCH_N          = 1'000'000;
constexpr int  BATCH_REPEATS    = 8;

static Tick<64> g_ticks[TICK_STREAM_LEN];
static SPSCRing<Tick<64>, EXECUTION_CORE_TICK_RING_SIZE> g_tick_ring;
static ExecutionCore<64> g_core;

static void build_tick_stream() {
    // Sawtooth around $60100, straddles the BG threshold so the gate
    // conditions exercise both sides — but with permission=1 and no
    // entries firing in scenario_c style, the steady-state path is
    // measured.
    for (int i = 0; i < TICK_STREAM_LEN; ++i) {
        double phase = (double)(i % 200);
        double price = 60050.0 + (phase < 100.0 ? phase : (200.0 - phase));
        g_ticks[i].price     = FPN_FromDouble<64>(price);
        g_ticks[i].volume    = FPN_FromDouble<64>(2.0);
        g_ticks[i].timestamp = (uint64_t)i * 1000;
        g_ticks[i].sequence  = (uint64_t)i;
    }
}

static void setup_core() {
    SPSCRing_Init(&g_tick_ring);
    ExecutionCore_Init(&g_core, 0, &g_tick_ring);
    GateParameters<64> p;
    GateParameters_Init(&p);
    p.bg_price_threshold   = FPN_FromDouble<64>(60000.0);  // never fires (price >= 60050)
    p.bg_volume_threshold  = FPN_Zero<64>();
    p.sg_take_profit_price = FPN_FromDouble<64>(60500.0);
    p.sg_stop_loss_price   = FPN_FromDouble<64>(59500.0);
    p.trade_size           = FPN_FromDouble<64>(0.01);
    p.strategy_id          = STRATEGY_SIMPLE_DIP;
    p.flags                = GATE_FLAG_TP_ENABLED | GATE_FLAG_SL_ENABLED;
    ExecutionCore_SetParameters(&g_core, p);
    // Bypass __atomic_store since we're single-threaded here
    g_core.permission = 1;
    // Critical: leave latency_stats DISABLED — we don't want the inline
    // rdtsc-on-every-tick tax during the batch measurement.
    CoreLatencyStats_Disable(&g_core.latency_stats);
}

// step() does NOT touch a volatile sink — the per-tick work is the only thing
// the optimizer should see. The caller escapes g_core.active once per batch
// (outside the timed loop) to keep dead-code elimination away.
__attribute__((always_inline))
static inline void step(int i) {
    const Tick<64>& t = g_ticks[(size_t)i & TICK_STREAM_MASK];
    ExecutionCore_Tick(&g_core, t);
}

//======================================================================================================
// step_cached: open-coded ExecutionCore_Tick variant that caches the parameter
// pack on the executor side. Re-reads the slot only when the seq counter has
// changed. In steady state (no concurrent producer write) this skips the
// 192-byte memcpy and only pays one acquire load + a compare.
//
// Cache state lives in static storage here for benchmarking. Production would
// embed it in the ExecutionCore struct.
//======================================================================================================
static GateParameters<64> g_cached_params;
static uint64_t g_cached_seq = (uint64_t)-1;  // sentinel: forces first-read miss

__attribute__((always_inline))
static inline void step_cached(int i) {
    const Tick<64>& t = g_ticks[(size_t)i & TICK_STREAM_MASK];

    // === Cached parameter slot read ===
    // Single acquire load, branch on seqcount equality. The producer's
    // RELEASE store on a new param push always advances seq by 2 (odd then
    // even), so cached_seq mismatch means "stale". On match the cached copy
    // is still valid because nothing was written since we last looked.
    uint64_t s1 = g_core.param_slot.seq.load(std::memory_order_acquire);
    if (__builtin_expect(s1 != g_cached_seq || (s1 & 1ULL), 0)) {
        // Slow path: do the full seqlock read into the cache. Identical
        // protocol to ParameterSlot_Read, just stores into cached_params.
        for (;;) {
            uint64_t s = g_core.param_slot.seq.load(std::memory_order_acquire);
            if ((s & 1ULL) != 0) { __builtin_ia32_pause(); continue; }
            uint64_t idx = (s >> 1) & 1ULL;
            g_cached_params = g_core.param_slot.buffers[idx];
            uint64_t s2 = g_core.param_slot.seq.load(std::memory_order_acquire);
            if (s == s2) { g_cached_seq = s; break; }
        }
    }

    // === Active-state TP/SL override (phase 14) ===
    GateParameters<64> params = g_cached_params;  // local copy so the compiler
                                                    // can put fields in regs
    if (g_core.active) {
        params.sg_take_profit_price = g_core.live_tp;
        params.sg_stop_loss_price   = g_core.live_sl;
    }

    // === Branchless gates ===
    uint64_t bg_fires = (uint64_t)BG_Evaluate(t, &params);
    uint64_t sg_fires = (uint64_t)SG_Evaluate(t.price, g_core.entry_price, &params);

    uint8_t  perm = __atomic_load_n(&g_core.permission, __ATOMIC_ACQUIRE);
    uint64_t can_enter = ((uint64_t)(~g_core.active & 1) & (uint64_t)perm & bg_fires) & 1ULL;
    uint64_t can_exit  = ((uint64_t)g_core.active & sg_fires) & 1ULL;

    if (can_enter | can_exit) {
        TradeEvent<64> event;
        event.price     = t.price;
        event.timestamp = t.timestamp;
        event.core_id   = g_core.core_id;
        event.type      = (uint8_t)(can_enter | (can_exit << 1));
        SPSCRing_TryPush(&g_core.event_ring, event);
    }

    if (can_enter) {
        g_core.entry_price = t.price;
        if (!FPN_IsZero(params.tp_pct)) {
            g_core.live_tp = FPN_Add(t.price, FPN_Mul(t.price, params.tp_pct));
        } else {
            g_core.live_tp = params.sg_take_profit_price;
        }
        if (!FPN_IsZero(params.sl_pct)) {
            g_core.live_sl = FPN_Sub(t.price, FPN_Mul(t.price, params.sl_pct));
        } else {
            g_core.live_sl = params.sg_stop_loss_price;
        }
    }

    g_core.active = (uint8_t)((g_core.active | can_enter) & ~can_exit);
}

//======================================================================================================
// step_cached_v2: like step_cached but does NOT copy the parameter pack to a
// stack-local. Reads fields directly from g_cached_params and uses CMOV-style
// selects to handle the active-override (live_tp/live_sl). Saves the 192-byte
// memcpy on every tick (which the compiler was emitting as 6 vmovdqa loads +
// 6 stores even with the cached path).
//======================================================================================================
__attribute__((always_inline))
static inline void step_cached_v2(int i) {
    const Tick<64>& t = g_ticks[(size_t)i & TICK_STREAM_MASK];

    // Cached parameter slot read (skip memcpy if seq unchanged)
    uint64_t s1 = g_core.param_slot.seq.load(std::memory_order_acquire);
    if (__builtin_expect(s1 != g_cached_seq || (s1 & 1ULL), 0)) {
        for (;;) {
            uint64_t s = g_core.param_slot.seq.load(std::memory_order_acquire);
            if ((s & 1ULL) != 0) { __builtin_ia32_pause(); continue; }
            uint64_t idx = (s >> 1) & 1ULL;
            g_cached_params = g_core.param_slot.buffers[idx];
            uint64_t s2 = g_core.param_slot.seq.load(std::memory_order_acquire);
            if (s == s2) { g_cached_seq = s; break; }
        }
    }

    // Read individual fields. The compiler keeps these in registers across
    // the tick body since g_cached_params is pure-read in the steady path.
    uint8_t  active = g_core.active;
    uint8_t  flags  = g_cached_params.flags;
    FPN<64>  bg_pt  = g_cached_params.bg_price_threshold;
    FPN<64>  bg_vt  = g_cached_params.bg_volume_threshold;

    // Phase 14 active override: select between cached params and live values.
    // CMOV-style — single cmp + 2 conditional moves. No branch.
    FPN<64> tp = active ? g_core.live_tp : g_cached_params.sg_take_profit_price;
    FPN<64> sl = active ? g_core.live_sl : g_cached_params.sg_stop_loss_price;

    // === Inlined BG ===
    uint64_t price_ok      = (uint64_t)FPN_LessThan(t.price, bg_pt);
    uint64_t volume_ok     = (uint64_t)FPN_GreaterThan(t.volume, bg_vt);
    uint64_t volume_req    = (uint64_t)((flags & GATE_FLAG_VOLUME_REQUIRED) != 0);
    uint64_t volume_check  = (volume_req & volume_ok) | (~volume_req & 1ULL);
    uint64_t bg_fires      = price_ok & volume_check;

    // === Inlined SG ===
    uint64_t tp_enabled    = (uint64_t)((flags & GATE_FLAG_TP_ENABLED) != 0);
    uint64_t sl_enabled    = (uint64_t)((flags & GATE_FLAG_SL_ENABLED) != 0);
    uint64_t tp_hit        = (uint64_t)FPN_GreaterThanOrEqual(t.price, tp);
    uint64_t sl_hit        = (uint64_t)FPN_LessThanOrEqual(t.price, sl);
    uint64_t sg_fires      = (tp_enabled & tp_hit) | (sl_enabled & sl_hit);

    uint8_t  perm = __atomic_load_n(&g_core.permission, __ATOMIC_ACQUIRE);
    uint64_t can_enter = ((uint64_t)(~active & 1) & (uint64_t)perm & bg_fires) & 1ULL;
    uint64_t can_exit  = ((uint64_t)active & sg_fires) & 1ULL;

    if (can_enter | can_exit) {
        TradeEvent<64> event;
        event.price     = t.price;
        event.timestamp = t.timestamp;
        event.core_id   = g_core.core_id;
        event.type      = (uint8_t)(can_enter | (can_exit << 1));
        SPSCRing_TryPush(&g_core.event_ring, event);
    }

    if (can_enter) {
        g_core.entry_price = t.price;
        FPN<64> tpp = g_cached_params.tp_pct;
        FPN<64> slp = g_cached_params.sl_pct;
        if (!FPN_IsZero(tpp)) {
            g_core.live_tp = FPN_Add(t.price, FPN_Mul(t.price, tpp));
        } else {
            g_core.live_tp = g_cached_params.sg_take_profit_price;
        }
        if (!FPN_IsZero(slp)) {
            g_core.live_sl = FPN_Sub(t.price, FPN_Mul(t.price, slp));
        } else {
            g_core.live_sl = g_cached_params.sg_stop_loss_price;
        }
    }

    g_core.active = (uint8_t)((active | can_enter) & ~can_exit);
}

//======================================================================================================
// step_floor: absolute minimum hot-path. Skips active override entirely, uses
// the cached params unconditionally. Tells us the structural floor for "any"
// per-tick check that does 4 FPN comparisons and an atomic load. The delta
// from v2 is the cost of the active-override CMOV path.
//======================================================================================================
__attribute__((always_inline))
static inline void step_floor(int i) {
    const Tick<64>& t = g_ticks[(size_t)i & TICK_STREAM_MASK];

    // No cache check — assume params never change. Just go straight to the gates.
    uint8_t  flags  = g_cached_params.flags;

    uint64_t price_ok      = (uint64_t)FPN_LessThan(t.price, g_cached_params.bg_price_threshold);
    uint64_t volume_ok     = (uint64_t)FPN_GreaterThan(t.volume, g_cached_params.bg_volume_threshold);
    uint64_t volume_req    = (uint64_t)((flags & GATE_FLAG_VOLUME_REQUIRED) != 0);
    uint64_t volume_check  = (volume_req & volume_ok) | (~volume_req & 1ULL);
    uint64_t bg_fires      = price_ok & volume_check;

    uint64_t tp_enabled    = (uint64_t)((flags & GATE_FLAG_TP_ENABLED) != 0);
    uint64_t sl_enabled    = (uint64_t)((flags & GATE_FLAG_SL_ENABLED) != 0);
    uint64_t tp_hit        = (uint64_t)FPN_GreaterThanOrEqual(t.price, g_cached_params.sg_take_profit_price);
    uint64_t sl_hit        = (uint64_t)FPN_LessThanOrEqual(t.price, g_cached_params.sg_stop_loss_price);
    uint64_t sg_fires      = (tp_enabled & tp_hit) | (sl_enabled & sl_hit);

    uint8_t  perm = __atomic_load_n(&g_core.permission, __ATOMIC_ACQUIRE);
    uint8_t  active = g_core.active;
    uint64_t can_enter = ((uint64_t)(~active & 1) & (uint64_t)perm & bg_fires) & 1ULL;
    uint64_t can_exit  = ((uint64_t)active & sg_fires) & 1ULL;
    g_core.active = (uint8_t)((active | can_enter) & ~can_exit);
}

//======================================================================================================
// step_just_perm: even tighter — the permission load + a single FPN compare.
// Tells us the cost of just "look at one price, decide nothing". The floor
// floor.
//======================================================================================================
__attribute__((always_inline))
static inline void step_just_perm(int i) {
    const Tick<64>& t = g_ticks[(size_t)i & TICK_STREAM_MASK];
    uint64_t fires = (uint64_t)FPN_LessThan(t.price, g_cached_params.bg_price_threshold);
    uint8_t  perm = __atomic_load_n(&g_core.permission, __ATOMIC_ACQUIRE);
    g_core.active = (uint8_t)(g_core.active | (fires & perm));
}

//======================================================================================================
// step_cached_v3: like v2 but uses an explicit branch for the active override
// instead of CMOV. The branch predicts perfectly in steady state (active=0)
// and the not-taken path skips the FPN load entirely.
//======================================================================================================
__attribute__((always_inline))
static inline void step_cached_v3(int i) {
    const Tick<64>& t = g_ticks[(size_t)i & TICK_STREAM_MASK];

    uint64_t s1 = g_core.param_slot.seq.load(std::memory_order_acquire);
    if (__builtin_expect(s1 != g_cached_seq || (s1 & 1ULL), 0)) {
        for (;;) {
            uint64_t s = g_core.param_slot.seq.load(std::memory_order_acquire);
            if ((s & 1ULL) != 0) { __builtin_ia32_pause(); continue; }
            uint64_t idx = (s >> 1) & 1ULL;
            g_cached_params = g_core.param_slot.buffers[idx];
            uint64_t s2 = g_core.param_slot.seq.load(std::memory_order_acquire);
            if (s == s2) { g_cached_seq = s; break; }
        }
    }

    uint8_t  active = g_core.active;
    uint8_t  flags  = g_cached_params.flags;

    // Active override via real branch — predicted not-taken in steady state.
    // Both branches end up with tp/sl in the same registers.
    FPN<64> tp, sl;
    if (__builtin_expect(active, 0)) {
        tp = g_core.live_tp;
        sl = g_core.live_sl;
    } else {
        tp = g_cached_params.sg_take_profit_price;
        sl = g_cached_params.sg_stop_loss_price;
    }

    // === Inlined BG ===
    uint64_t price_ok      = (uint64_t)FPN_LessThan(t.price, g_cached_params.bg_price_threshold);
    uint64_t volume_ok     = (uint64_t)FPN_GreaterThan(t.volume, g_cached_params.bg_volume_threshold);
    uint64_t volume_req    = (uint64_t)((flags & GATE_FLAG_VOLUME_REQUIRED) != 0);
    uint64_t volume_check  = (volume_req & volume_ok) | (~volume_req & 1ULL);
    uint64_t bg_fires      = price_ok & volume_check;

    // === Inlined SG ===
    uint64_t tp_enabled    = (uint64_t)((flags & GATE_FLAG_TP_ENABLED) != 0);
    uint64_t sl_enabled    = (uint64_t)((flags & GATE_FLAG_SL_ENABLED) != 0);
    uint64_t tp_hit        = (uint64_t)FPN_GreaterThanOrEqual(t.price, tp);
    uint64_t sl_hit        = (uint64_t)FPN_LessThanOrEqual(t.price, sl);
    uint64_t sg_fires      = (tp_enabled & tp_hit) | (sl_enabled & sl_hit);

    uint8_t  perm = __atomic_load_n(&g_core.permission, __ATOMIC_ACQUIRE);
    uint64_t can_enter = ((uint64_t)(~active & 1) & (uint64_t)perm & bg_fires) & 1ULL;
    uint64_t can_exit  = ((uint64_t)active & sg_fires) & 1ULL;

    if (__builtin_expect(can_enter | can_exit, 0)) {
        TradeEvent<64> event;
        event.price     = t.price;
        event.timestamp = t.timestamp;
        event.core_id   = g_core.core_id;
        event.type      = (uint8_t)(can_enter | (can_exit << 1));
        SPSCRing_TryPush(&g_core.event_ring, event);

        if (can_enter) {
            g_core.entry_price = t.price;
            FPN<64> tpp = g_cached_params.tp_pct;
            if (!FPN_IsZero(tpp))
                g_core.live_tp = FPN_Add(t.price, FPN_Mul(t.price, tpp));
            else
                g_core.live_tp = g_cached_params.sg_take_profit_price;
            FPN<64> slp = g_cached_params.sl_pct;
            if (!FPN_IsZero(slp))
                g_core.live_sl = FPN_Sub(t.price, FPN_Mul(t.price, slp));
            else
                g_core.live_sl = g_cached_params.sg_stop_loss_price;
        }
    }

    g_core.active = (uint8_t)((active | can_enter) & ~can_exit);
}

int main() {
    build_tick_stream();
    setup_core();

    uint64_t tsc_hz = calibrate_tsc_hz(200'000'000ULL);
    printf("[bench_batch_floor] TSC = %.4f GHz\n", (double)tsc_hz / 1e9);

    // ---- per-call bracketed (Heisenberg in, comparable to scenario C) ----
    // Per-call uses sample() which already brackets each call with rdtsc.
    // The lambda calls step() which has no volatile sink — that's fine
    // because rdtsc_start/end act as compiler memory barriers.
    auto per_call = sample_with_warmup(WARMUP_N, PER_CALL_SAMPLES,
        [&](int i) { step(i); });
    Stats pc = compute_stats(per_call, 0.001);  // drop top 0.1% for kernel noise

    printf("\n--- per-call bracketed (rdtsc around every tick) ---\n");
    print_table_header_ns();
    print_row_ns("ExecutionCore_Tick (per-call)", pc, tsc_hz);
    printf("\n");

    // ---- batch bracketed (rdtsc amortized across N iterations) ----
    // Repeat the batch BATCH_REPEATS times so we can see variance.
    std::vector<double> batch_per_tick_ns;
    batch_per_tick_ns.reserve(BATCH_REPEATS);
    uint64_t min_batch_cycles = UINT64_MAX;
    uint64_t max_batch_cycles = 0;

    // Warmup the batch path itself
    for (int w = 0; w < 3; ++w) {
        for (int i = 0; i < 50'000; ++i) step(i);
    }

    for (int r = 0; r < BATCH_REPEATS; ++r) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH_N; ++i) step(i);
        // Force the optimizer to keep core->active live by reading it
        // through an asm clobber. Costs nothing inside the timed region
        // because it lives AFTER the rdtsc_end below — but we put it
        // before so the optimizer can't move the loop's stores into it.
        asm volatile("" :: "r"(g_core.active) : "memory");
        uint64_t t1 = rdtsc_end();
        uint64_t cycles = t1 - t0;
        double per_tick_cycles = (double)cycles / (double)BATCH_N;
        double per_tick_ns = per_tick_cycles * 1e9 / (double)tsc_hz;
        batch_per_tick_ns.push_back(per_tick_ns);
        if (cycles < min_batch_cycles) min_batch_cycles = cycles;
        if (cycles > max_batch_cycles) max_batch_cycles = cycles;
    }
    (void)min_batch_cycles; (void)max_batch_cycles;

    std::sort(batch_per_tick_ns.begin(), batch_per_tick_ns.end());
    double batch_min = batch_per_tick_ns.front();
    double batch_med = batch_per_tick_ns[batch_per_tick_ns.size() / 2];
    double batch_max = batch_per_tick_ns.back();

    printf("--- batch bracketed (rdtsc once around %d ticks, %d repeats) ---\n",
           BATCH_N, BATCH_REPEATS);
    printf("ORIGINAL ExecutionCore_Tick (full memcpy of 192B every tick)\n");
    printf("repeat        ns/tick\n");
    printf("------        -------\n");
    for (size_t i = 0; i < batch_per_tick_ns.size(); ++i) {
        printf("  %2zu          %6.2f\n", i, batch_per_tick_ns[i]);
    }
    printf("batch min  : %6.2f ns/tick\n", batch_min);
    printf("batch med  : %6.2f ns/tick\n", batch_med);
    printf("batch max  : %6.2f ns/tick\n", batch_max);
    printf("\n");

    // ---- batch bracketed CACHED variant (skips memcpy on seq match) ----
    // Warmup the cached path so the first run isn't biased by cold cache /
    // first-time params load.
    g_cached_seq = (uint64_t)-1;
    for (int w = 0; w < 3; ++w) {
        for (int i = 0; i < 50'000; ++i) step_cached(i);
    }

    std::vector<double> batch_cached_ns;
    batch_cached_ns.reserve(BATCH_REPEATS);
    for (int r = 0; r < BATCH_REPEATS; ++r) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH_N; ++i) step_cached(i);
        asm volatile("" :: "r"(g_core.active) : "memory");
        uint64_t t1 = rdtsc_end();
        uint64_t cycles = t1 - t0;
        double per_tick_ns = (double)cycles * 1e9 / (double)tsc_hz / (double)BATCH_N;
        batch_cached_ns.push_back(per_tick_ns);
    }
    std::sort(batch_cached_ns.begin(), batch_cached_ns.end());
    double cached_min = batch_cached_ns.front();
    double cached_med = batch_cached_ns[batch_cached_ns.size() / 2];
    double cached_max = batch_cached_ns.back();

    printf("CACHED variant (re-read only on seq change)\n");
    printf("repeat        ns/tick\n");
    printf("------        -------\n");
    for (size_t i = 0; i < batch_cached_ns.size(); ++i) {
        printf("  %2zu          %6.2f\n", i, batch_cached_ns[i]);
    }
    printf("cached min : %6.2f ns/tick\n", cached_min);
    printf("cached med : %6.2f ns/tick\n", cached_med);
    printf("cached max : %6.2f ns/tick\n", cached_max);
    printf("\n");
    printf("savings    : %6.2f ns/tick (cached_min vs batch_min)\n",
           batch_min - cached_min);
    printf("\n");

    // ---- batch bracketed CACHED V2: no local memcpy ----
    g_cached_seq = (uint64_t)-1;
    for (int w = 0; w < 3; ++w) {
        for (int i = 0; i < 50'000; ++i) step_cached_v2(i);
    }
    std::vector<double> batch_v2_ns;
    batch_v2_ns.reserve(BATCH_REPEATS);
    for (int r = 0; r < BATCH_REPEATS; ++r) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH_N; ++i) step_cached_v2(i);
        asm volatile("" :: "r"(g_core.active) : "memory");
        uint64_t t1 = rdtsc_end();
        double per_tick_ns = (double)(t1 - t0) * 1e9 / (double)tsc_hz / (double)BATCH_N;
        batch_v2_ns.push_back(per_tick_ns);
    }
    std::sort(batch_v2_ns.begin(), batch_v2_ns.end());
    double v2_min = batch_v2_ns.front();
    double v2_med = batch_v2_ns[batch_v2_ns.size() / 2];
    double v2_max = batch_v2_ns.back();

    printf("CACHED V2 (no local copy, direct field reads, CMOV active select)\n");
    printf("repeat        ns/tick\n");
    printf("------        -------\n");
    for (size_t i = 0; i < batch_v2_ns.size(); ++i) {
        printf("  %2zu          %6.2f\n", i, batch_v2_ns[i]);
    }
    printf("v2 min     : %6.2f ns/tick\n", v2_min);
    printf("v2 med     : %6.2f ns/tick\n", v2_med);
    printf("v2 max     : %6.2f ns/tick\n", v2_max);
    printf("\n");
    printf("v2 vs orig : %6.2f ns saved\n", batch_min - v2_min);
    printf("v2 vs cache: %6.2f ns saved\n", cached_min - v2_min);
    printf("\n");

    // ---- batch bracketed CACHED V3: branch for active, fold entry into rare ----
    g_cached_seq = (uint64_t)-1;
    for (int w = 0; w < 3; ++w) {
        for (int i = 0; i < 50'000; ++i) step_cached_v3(i);
    }
    std::vector<double> batch_v3_ns;
    batch_v3_ns.reserve(BATCH_REPEATS);
    for (int r = 0; r < BATCH_REPEATS; ++r) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH_N; ++i) step_cached_v3(i);
        asm volatile("" :: "r"(g_core.active) : "memory");
        uint64_t t1 = rdtsc_end();
        double per_tick_ns = (double)(t1 - t0) * 1e9 / (double)tsc_hz / (double)BATCH_N;
        batch_v3_ns.push_back(per_tick_ns);
    }
    std::sort(batch_v3_ns.begin(), batch_v3_ns.end());
    double v3_min = batch_v3_ns.front();
    double v3_med = batch_v3_ns[batch_v3_ns.size() / 2];
    double v3_max = batch_v3_ns.back();

    printf("CACHED V3 (branch for active, entry-update in rare branch)\n");
    printf("repeat        ns/tick\n");
    printf("------        -------\n");
    for (size_t i = 0; i < batch_v3_ns.size(); ++i) {
        printf("  %2zu          %6.2f\n", i, batch_v3_ns[i]);
    }
    printf("v3 min     : %6.2f ns/tick\n", v3_min);
    printf("v3 med     : %6.2f ns/tick\n", v3_med);
    printf("v3 max     : %6.2f ns/tick\n", v3_max);
    printf("v3 vs v2   : %6.2f ns saved\n", v2_min - v3_min);
    printf("v3 vs orig : %6.2f ns saved\n", batch_min - v3_min);
    printf("\n");

    // ---- floor: gates only, no cache check, no active override ----
    for (int w = 0; w < 3; ++w) {
        for (int i = 0; i < 50'000; ++i) step_floor(i);
    }
    std::vector<double> floor_ns;
    floor_ns.reserve(BATCH_REPEATS);
    for (int r = 0; r < BATCH_REPEATS; ++r) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH_N; ++i) step_floor(i);
        asm volatile("" :: "r"(g_core.active) : "memory");
        uint64_t t1 = rdtsc_end();
        double per_tick_ns = (double)(t1 - t0) * 1e9 / (double)tsc_hz / (double)BATCH_N;
        floor_ns.push_back(per_tick_ns);
    }
    std::sort(floor_ns.begin(), floor_ns.end());
    double floor_min = floor_ns.front();

    printf("FLOOR (gates + perm load only, no cache, no active override)\n");
    for (size_t i = 0; i < floor_ns.size(); ++i) printf("  %2zu  %6.2f\n", i, floor_ns[i]);
    printf("floor min  : %6.2f ns/tick\n", floor_min);
    printf("\n");

    // ---- floor floor: 1 FPN cmp + 1 atomic load ----
    for (int w = 0; w < 3; ++w) {
        for (int i = 0; i < 50'000; ++i) step_just_perm(i);
    }
    std::vector<double> ff_ns;
    ff_ns.reserve(BATCH_REPEATS);
    for (int r = 0; r < BATCH_REPEATS; ++r) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH_N; ++i) step_just_perm(i);
        asm volatile("" :: "r"(g_core.active) : "memory");
        uint64_t t1 = rdtsc_end();
        double per_tick_ns = (double)(t1 - t0) * 1e9 / (double)tsc_hz / (double)BATCH_N;
        ff_ns.push_back(per_tick_ns);
    }
    std::sort(ff_ns.begin(), ff_ns.end());
    double ff_min = ff_ns.front();
    printf("ABS FLOOR (1 FPN cmp + 1 perm load + active update)\n");
    for (size_t i = 0; i < ff_ns.size(); ++i) printf("  %2zu  %6.2f\n", i, ff_ns[i]);
    printf("abs floor  : %6.2f ns/tick\n", ff_min);
    printf("\n");
    printf("=== summary (sorted) ===\n");
    printf("orig (full memcpy)        : %6.2f ns/tick\n", batch_min);
    printf("cached (skip memcpy)      : %6.2f ns/tick\n", cached_min);
    printf("cached_v2 (no local copy) : %6.2f ns/tick\n", v2_min);
    printf("cached_v3 (branch active) : %6.2f ns/tick\n", v3_min);
    printf("floor (no override)       : %6.2f ns/tick\n", floor_min);
    printf("abs floor (1 cmp+perm)    : %6.2f ns/tick\n", ff_min);
    printf("\n");

    // ---- compare ----
    double per_call_p50_ns = cycles_to_ns(pc.p50, tsc_hz);
    double per_call_min_ns = cycles_to_ns(pc.min, tsc_hz);
    printf("--- comparison ---\n");
    printf("per-call min p50 : %6.2f ns  (rdtsc bracket + work)\n", per_call_p50_ns);
    printf("per-call min     : %6.2f ns  (best per-call sample)\n", per_call_min_ns);
    printf("batch min        : %6.2f ns  (work only, rdtsc amortized)\n", batch_min);
    printf("rdtsc tax (p50)  : %6.2f ns  (per-call p50 minus batch min)\n",
           per_call_p50_ns - batch_min);
    printf("\n");

    // sanity: ensure the loop ran (active should still be 0 since BG never fires)
    printf("[bench_batch_floor] core.active = %u (sanity, loop executed)\n",
           (unsigned)g_core.active);
    return 0;
}
