// rdtsc.hpp — TSC-based cycle counter with proper serialization fences
//
// Why these specific fence patterns:
//   - rdtsc itself is NOT a serializing instruction. The CPU can reorder loads
//     and stores around it, which means a naive `rdtsc; code; rdtsc` measurement
//     can include or exclude work that wasn't actually inside the timed region.
//   - Intel's recommended pattern (from "How to Benchmark Code Execution Times
//     on Intel IA-32 and IA-64 Instruction Set Architectures", Gabriele Paoloni):
//       BEFORE the measured code: mfence + lfence + rdtsc
//       AFTER the measured code:  rdtscp + lfence
//     The mfence drains the store buffer, the lfence drains in-flight loads
//     before rdtsc reads the counter, rdtscp serializes against subsequent
//     instructions but allows them to start (so we capture the right end
//     boundary), and the trailing lfence prevents any instruction after our
//     measurement window from leaking into the timed region.
//
// Why we calibrate at runtime:
//   - constant_tsc means the TSC counts at a fixed rate, but that rate is NOT
//     the same as the CPU's current frequency. On modern Intel it's typically
//     the nominal/base frequency, but the exact value varies by CPU. The
//     reliable way to convert ticks to nanoseconds is to measure: count ticks
//     over a known wall-clock interval and divide.
//   - We use CLOCK_MONOTONIC_RAW (not CLOCK_MONOTONIC) because raw is unaffected
//     by NTP adjustments, which matters for sub-millisecond accuracy.

#pragma once

#include <cstdint>
#include <ctime>

namespace experiment {

// Read TSC at the START of a measurement region.
// mfence drains stores, lfence drains loads, rdtsc then sees a clean state.
static inline uint64_t rdtsc_start() {
    uint32_t hi, lo;
    asm volatile (
        "mfence\n\t"
        "lfence\n\t"
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        :: "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

// Read TSC at the END of a measurement region.
// rdtscp serializes against the preceding code (so all measured work has
// retired), then lfence prevents post-measurement instructions from leaking
// backwards into the window.
static inline uint64_t rdtsc_end() {
    uint32_t hi, lo, aux;
    asm volatile (
        "rdtscp\n\t"
        "lfence\n\t"
        : "=a"(lo), "=d"(hi), "=c"(aux)
        :: "memory"
    );
    (void)aux;
    return ((uint64_t)hi << 32) | lo;
}

// Calibrate TSC frequency by counting ticks over a known wall-clock interval.
// Returns ticks-per-second. Sleeps for the calibration window, so don't call
// this on the hot path — call it once at startup.
static inline uint64_t calibrate_tsc_hz(uint64_t calibration_ns = 100'000'000ULL) {
    timespec ts_start{}, ts_end{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start);
    uint64_t tsc_start = rdtsc_start();

    timespec sleep_dur{};
    sleep_dur.tv_sec  = (time_t)(calibration_ns / 1'000'000'000ULL);
    sleep_dur.tv_nsec = (long)(calibration_ns % 1'000'000'000ULL);
    nanosleep(&sleep_dur, nullptr);

    uint64_t tsc_end = rdtsc_end();
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts_end);

    uint64_t elapsed_ns =
        ((uint64_t)ts_end.tv_sec - (uint64_t)ts_start.tv_sec) * 1'000'000'000ULL +
        ((uint64_t)ts_end.tv_nsec - (uint64_t)ts_start.tv_nsec);
    uint64_t elapsed_tsc = tsc_end - tsc_start;

    if (elapsed_ns == 0) return 0;
    return (elapsed_tsc * 1'000'000'000ULL) / elapsed_ns;
}

// Convert TSC ticks to nanoseconds given a calibrated frequency.
static inline double cycles_to_ns(uint64_t cycles, uint64_t tsc_hz) {
    if (tsc_hz == 0) return 0.0;
    return (double)cycles * 1e9 / (double)tsc_hz;
}

} // namespace experiment
