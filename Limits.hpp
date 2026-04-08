#pragma once
// centralized compile-time constants
// change HERE only — all files include this for consistent bounds

#define MAX_PORTFOLIO_POSITIONS 16
#define CANDLE_HISTORY_MAX 4096
#define CANDLE_INTERVAL_DEFAULT 60
#define MAX_TRADE_HISTORY 512
#define MAX_DATA_FILES 256
#define LOG_BUFFER_SIZE 32768

// per-core sharding (phase 04+)
// MAX_EXECUTION_CORES caps the number of execution cores the controller can
// register. one core per pinned CPU. core_id maps directly to portfolio slot.
// MAX_EVENTS_PER_DRAIN_PER_CORE caps how many events one drain pass will pull
// from a single core's event ring before moving to the next core. prevents one
// chatty core from starving the others (pitfall P4.1).
#define MAX_EXECUTION_CORES 16
#define MAX_EVENTS_PER_DRAIN_PER_CORE 16
