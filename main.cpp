// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [TICK TRADER ENGINE]
//======================================================================================================
// main event loop - wires the Binance data stream to the execution pipeline
// single-threaded, poll-based: BinanceStream -> BuyGate -> PositionExitGate -> PortfolioController
//
// the engine runs identically whether data is arriving or not - poll() timeout ensures
// exit gates are always checked against last known price
//
// burst drain: when multiple frames buffer between poll cycles (volatile markets),
// we drain ALL of them, running exit gates on each intermediate price so TP/SL triggers
// arent missed in a burst. BuyGate only runs on the final/freshest price
//
// session lifecycle: 24-hour cycle with clean wind-down, position close, and reconnect
// reconnect procedure is airtight - verifies bitmap is zero before proceeding
//======================================================================================================
#include <locale.h>   // .E.0.1: LC_NUMERIC=C boot pin (locale-determinism class close)
#include "DataStream/BinanceCrypto.hpp"
#include "CoreFrameworks/Notify.hpp"
// Phase 8b — g_notify is a C++17 inline variable defined in Notify.hpp,
// nullptr by default. Live engine assigns &g_notify_state in main() after
// NotifyState_Init when cfg.notify_enabled=1 (lands in c3+c4).
#include "CoreFrameworks/EngineSharded.hpp"
#include "CoreFrameworks/SystemInit.hpp"  // v5.11.0.A — engine_set_mxcsr_ftz_daz

#ifdef USE_IMGUI_GUI
#include "GUI/CandleAccumulator.hpp"
#include "GUI/GuiThread.hpp"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>     // v5.11.0.B — mlockall
#include <sys/resource.h> // v5.11.0.B — getrlimit / RLIMIT_MEMLOCK
#include <errno.h>        // v5.11.0.B — strerror(errno) on mlockall fail
#include <string.h>       // v5.11.0.B — strerror

#ifdef LATENCY_PROFILING
#include <x86intrin.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

constexpr unsigned FP = 64;


//======================================================================================================
// [MAIN]
//======================================================================================================
int main(int argc, char *argv[]) {
    // v5.11.0.A — Set FTZ/DAZ as the FIRST thing in main(). Subnormal stalls
    // cost up to 100x FPU throughput (microcode trap); critical for HFT
    // determinism. Linux pthread_create inherits MXCSR, so this covers all
    // slow-path threads. Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 12.3.
    tt::engine_set_mxcsr_ftz_daz();

    // .E.0.1 locale-determinism class close: pin LC_NUMERIC=C process-wide at boot,
    // BEFORE any float parse (cfg load at :139+). setlocale is process-global so every
    // pthread inherits it — makes the formerly-phantom "engine boot pins this"
    // (NodeModelZoo.hpp:2845) actually TRUE. Headless engine: sole pin. engine_gui:
    // re-pinned after SDL_Init (GuiThread) since SDL/X11 can reset LC_*.
    setlocale(LC_NUMERIC, "C");

    fprintf(stderr, "FoxML_Trader_v2 — Copyright (c) 2026 Jennifer Lewis. All rights reserved.\n");
    fprintf(stderr, "Licensed under AGPL-3.0-or-later. Commercial license: jenn.lewis5789@gmail.com\n\n");

    const char *cfg_path = (argc > 1) ? argv[1] : "engine.cfg";

    //==================================================================================================
    // load configs
    //==================================================================================================
    BinanceConfig bcfg       = BinanceConfig_Load(cfg_path);
    ControllerConfig<FP> ccfg = ControllerConfig_Load<FP>(cfg_path);

    // create logging directory — all runtime files go here (rm -rf logging/* for clean start)
    mkdir("logging", 0755); // silently succeeds if already exists

    // auto-redirect stderr to log file — must happen BEFORE sharded dispatch
    // so the Engine Log panel can read from logging/engine.log
    if (bcfg.log_file[0]) {
        char log_path[300], prev[304];
        snprintf(log_path, sizeof(log_path), "logging/%s", bcfg.log_file);
        snprintf(prev, sizeof(prev), "%s.1", log_path);
        rename(log_path, prev); // silently fails if no existing log
        FILE *lf = freopen(log_path, "w", stderr);
        if (!lf) {
            perror("freopen log_file");
        } else {
            setvbuf(stderr, NULL, _IOLBF, 0); // line-buffered so tail -f works
        }
    }

    //==================================================================================================
    // [v5.11.0.B — LOCK MEMORY PAGES INTO RAM]
    //==================================================================================================
    // Audit: LATENCY_OPTIMIZATION_AUDIT.md Part 12.2 — page swap stalls cost
    // hundreds of microseconds. mlockall locks all pages into physical RAM,
    // preventing the kernel from swapping out critical execution memory.
    //
    // Ordering matters: this fires AFTER freopen(log_file) above, so a fatal
    // mlockall failure prints to logging/engine.log rather than terminal
    // stderr (where headless / systemd / nohup operators wouldn't see it).
    // Trade-off: cfg-parsing memory at lines ~128-129 isn't locked, but cfg
    // is parsed-and-discarded outside the hot path; not a regression.
    //
    // Failure modes:
    //   1. RLIMIT_MEMLOCK soft limit too low → mlockall returns EAGAIN.
    //      We probe the limit first and emit a clear WARN before attempting.
    //   2. Process lacks CAP_IPC_LOCK on non-root → mlockall returns EPERM.
    //      Operator must run with appropriate caps or as root.
    // An HFT engine that can't lock its memory is a fail-fast condition
    // (per HFT-suggestion annotation in plan).
    //==================================================================================================
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
            // Heuristic: need at least the engine's typical resident size.
            // 256 MB is generous for current sizing (zoo + scaler + bandit
            // state + cfg + ring buffers); alarm if soft limit is below.
            const rlim_t kMinMemlock = 256ULL * 1024 * 1024;
            if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < kMinMemlock) {
                fprintf(stderr,
                    "[v5.11.0.B] WARNING: RLIMIT_MEMLOCK soft limit is %llu bytes, "
                    "want >= %llu. mlockall may fail. Raise via "
                    "`ulimit -l unlimited` or /etc/security/limits.conf.\n",
                    (unsigned long long)rl.rlim_cur,
                    (unsigned long long)kMinMemlock);
            }
        }
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            const int level_required = (ccfg.require_mlockall != 0);
            fprintf(stderr,
                "[v5.11.0.B] %s: mlockall failed: %s. "
                "Engine cannot guarantee deterministic latency without locked pages. "
                "Run with CAP_IPC_LOCK or as root, and ensure RLIMIT_MEMLOCK is raised "
                "(`ulimit -l unlimited` for this shell, or /etc/security/limits.conf "
                "for persistent config).%s\n",
                level_required ? "FATAL" : "WARN",
                strerror(errno),
                level_required ? "" :
                    " Continuing because require_mlockall=0 (laptop/dev mode).");
            if (level_required) return 1;
        }
    }

    //==================================================================================================
    // Phase 13+ dispatch: SHARDED is the production engine and is now the DEFAULT.
    // Legacy single-threaded mode is kept as a benchmark/regression baseline but is
    // DEPRECATED — see CLAUDE.md "Cross-Mode Init Placement" invariant. Adding
    // features in main.cpp's post-dispatch loop = silent production gap (the
    // sharded path won't see them). New features should land in:
    //   - EngineSharded.hpp (sharded-only setup / per-core init)
    //   - CoreFrameworks/EventLoopState (cross-core dispatch)
    //   - CoreFrameworks/OrderManager (OMS HandleFill — fee math + counters)
    //==================================================================================================
    // WIP2d-1.A — per-core symbol axis (partial advance of .F.4c.3.A; uniformity check
    // + bridge to BinanceConfig.symbol). Operator can set core_<N>_symbol=BTCUSDT in
    // engine.cfg; this overrides binance.cfg's symbol field if uniformity holds. Multi-
    // symbol DataStream not yet supported — boot fails with clear error if cores have
    // mismatched non-empty symbols. Empty = no override (binance.cfg's symbol drives).
    {
        const char* primary_symbol = ccfg.node_symbol[0];
        bool any_set = (primary_symbol[0] != '\0');
        for (uint16_t c = 1; c < ccfg.num_execution_nodes && c < 16; ++c) {
            const char* sc = ccfg.node_symbol[c];
            if (sc[0] != '\0') {
                any_set = true;
                if (primary_symbol[0] == '\0') {
                    primary_symbol = sc;  // first non-empty wins as primary
                } else if (strcmp(sc, primary_symbol) != 0) {
                    fprintf(stderr,
                        "[boot] FATAL: per-core symbols differ (core %d='%s' vs primary='%s'); "
                        "multi-symbol DataStream not yet supported. Set all core_<N>_symbol= "
                        "identical OR leave all empty (binance.cfg's symbol drives).\n",
                        (int)c, sc, primary_symbol);
                    return 1;
                }
            }
        }
        if (any_set) {
            strncpy(bcfg.symbol, primary_symbol, sizeof(bcfg.symbol) - 1);
            bcfg.symbol[sizeof(bcfg.symbol) - 1] = '\0';
            fprintf(stderr, "[boot] per-core symbol override: BinanceConfig.symbol='%s' "
                            "(from engine.cfg core_<N>_symbol=)\n", bcfg.symbol);
        }
    }

    // NEW-1/D-218 — HARD-REFUSE a contradictory capital config BEFORE any engine dispatch
    // (use_real_money=1 conflicting with an explicit non-LIVE trading_mode; Load flagged it).
    // Ambiguous capital intent on a SAFETY_CRITICAL field must not boot — covers sharded + legacy.
    if (ccfg.live_capital_cfg_conflict) {
        fprintf(stderr, "[ENGINE] FATAL: contradictory capital config (use_real_money vs trading_mode) "
                        "-> boot REFUSED. Resolve engine.cfg (see the [cfg] FATAL above).\n");
        return 1;
    }

    // Sharded is the sole engine path (.E.1.1 removed engine_mode + legacy single_core).
    // v5.15.5.C.3 Phase 7.B — runtime bench gate boot dispatch. Two template
    // instantiations of EngineSharded_Run exist in the binary: <64, false>
    // (production; zero bench cost) and <64, true> (bench mode; emits drainer cycle
    // latency histogram + stderr summary at shutdown). Operator flips
    // cfg.oms_bench_enabled=1 in engine.cfg to opt in; default 0 keeps production behavior.
    if (ccfg.oms_bench_enabled) {
        tt::EngineSharded_Run<64, /*BENCH=*/true>(ccfg, bcfg);
    } else {
        tt::EngineSharded_Run<64, /*BENCH=*/false>(ccfg, bcfg);
    }
    return 0;
}
