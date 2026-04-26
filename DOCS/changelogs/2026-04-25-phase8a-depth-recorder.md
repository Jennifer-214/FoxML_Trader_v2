# 2026-04-25 (evening) — Phase 8a: Depth Feed activation + Recording

Branch: `experiment/live-readiness`. Second phase after `phase5d-regression-tests`.
Continues from tag `phase5d-regression-tests`. Mid-implementation discovery
expanded scope (4 → 6 commits) — see "Scope expansion" below.

Six commits land here, each tagged `phase8a-c1` … `phase8a-c6`. Test
binary lands in c7 (separate commit per test scope).

## What ships

The engine can now actually subscribe to `@depth5@100ms`, feed real
book imbalance into the buy gate, and persist depth snapshots for
future replay/audit.

**Behavior matrix** (defaults preserve pre-Phase-8a behavior):

| `depth_enabled` | `record_depth` | `min_book_imbalance` | What happens |
|---|---|---|---|
| 0 (default) | (any) | (any) | Pre-Phase-8a: no thread, gate inert, no recording |
| 1 | 0 (default) | 0 (default) | Thread runs, `book_imbalance` flows, gate inert (no behavior change). No recording. |
| 1 | 0 | > 0 | Thread runs, `book_imbalance` flows, **gate fires on real data — behavioral change**. No recording. |
| 1 | 1 | (any) | Thread runs + recording active. CSV at `data/{SYMBOL}/depth/YYYY-MM-DD.csv` |

Default cfg = pre-Phase-8a behavior. Live trading unaffected unless user
opts in.

## Scope expansion mid-implementation

The original Phase 8a plan (`plans/phase8a-depth-recorder.md`) said:

> "Live depth infrastructure exists and works:
> - subscribes to `@depth5@100ms` when `depth_enabled=1`
> - parsed snapshots populate `book_imbalance` field via atomic swap"

Discovered while starting the original c3 (the "wire recorder into
depth_thread_fn" commit) that **nothing called `pthread_create` with
`depth_thread_fn` anywhere in the codebase**. `cfg.depth_enabled` was
parsed but did nothing. `book_imbalance` was initialized to 0 in
`PortfolioController_Init` and stayed there. The gate at
`PortfolioController.hpp:1600` read dead data.

The original plan would have produced an empty CSV — recorder wired
into a thread that never ran.

**Restructure:** added 2 commits before the recorder integration:
- New c3: activate the depth thread (`pthread_create` + cleanup)
- New c4: feed `book_imbalance` from `DepthSharedState` into controller

Original c3+c4 became c5+c6.

## Commits

### c1 (`afcab06`) — BookSnapshot extension

Added `last_update_id` (Binance `lastUpdateId`) and `timestamp_us`
(local CLOCK_REALTIME stamp) to `BookSnapshot<F>`. Parser extracts
`lastUpdateId` from JSON. Thread sets `timestamp_us` after successful
parse before atomic swap. No consumer reads either yet (c2 introduces
the recorder that uses both for gap detection).

Anti-drift: no `sizeof(BookSnapshot)` / `alignof(BookSnapshot)`
assumptions in the codebase — adding 16 bytes is safe.

### c2 (`b50e6b3`) — DepthRecorder primitive

Sibling of TickRecorder. Header-only, allocation-free per-snapshot
Write path, 256-snapshot fflush cadence. Daily rotation, auto-prune
via `max_days`.

**Gap detection lives INSIDE `DepthRecorder_Write`**, not in
`depth_thread_fn` (this is amendment #1 vs. the original plan, which
had it in the thread but the test sidecar tested it in the recorder —
moving it to the recorder eliminates that inconsistency).

**Corrected gap algorithm (master plan errata):** original plan
treated `cur_id > last_seen + 1` as a gap. But `@depth5@100ms`
snapshots normally jump `lastUpdateId` by 50-500 between messages
(book updates faster than the 10Hz feed). The "+1" check would
false-positive every message. Real gaps look like:

- `cur_id < last_seen` (backward — reconnect to a stale snapshot)
- `wallclock_now_us - last_seen_wallclock_us > 2_000_000` (≥2s
  silence — connection dead but socket still open)
- explicit `_LogGap` call from disconnect site

`_LogGap` zeros `last_seen_id` so the post-reconnect first `_Write`
skips its internal gap check (no double-flagging).

### c3 (`10ef9d2`) — Activate depth thread

`main.cpp` now actually calls `pthread_create(depth_thread_fn, ...)`
when `cfg.depth_enabled=1`. Picks the same host (testnet /
binance.us / production) the trade stream uses. On shutdown, signals
`quit_requested` (atomic with RELEASE) and `pthread_join` — depth
thread polls the flag at the top of its loop with ACQUIRE, next
200ms cycle picks it up.

Init failure is non-fatal — log + continue without depth.

Pre-audits passed:
- OpenSSL: `OPENSSL_init_ssl(0, NULL)` is safe to call multiple times
  (per BinanceCrypto.hpp:475 comment). Each WS connection has its own
  `SSL_CTX` via `ws_ssl_setup`. No conflict.
- Shutdown pattern matches existing TUI thread join.

### c4 (`79ce65c`) — Feed book_imbalance from depth into controller

The buy gate at `PortfolioController.hpp:1600` reads
`ctrl->book_imbalance` when `min_book_imbalance > 0`. Pre-c4 this was
always 0 (initialized to zero in `PortfolioController_Init`, never
updated). Now: every tick, before `PortfolioController_Tick`, an
ACQUIRE load on `depth_shared.active_idx` pairs with the depth
thread's RELEASE store, and we copy the active snapshot's
`imbalance` into `ctrl.book_imbalance`.

Default `min_book_imbalance=0` keeps the gate inert. **Setting
`min_book_imbalance > 0` with `depth_enabled=1` now actually gates
buys on real orderbook depth — was inert before c4.**

Cost: one ACQUIRE load + one struct field assignment per tick.
Branch is constant for the run (compiler hoists). No hot-path
impact in practice — actual gate decision still happens in slow path
inside `PortfolioController_Tick`.

### c5 (`460eeeb`) — Wire DepthRecorder into depth_thread_fn

`DepthSharedState` gets a `DepthRecorder *recorder` field
(forward-declared; full type via late `#include "DepthRecorder.hpp"`
inside BinanceDepth.hpp to break the include cycle). When non-null
and parse succeeds, depth_thread_fn calls `DepthRecorder_Write` —
recorder does its own internal gap detection. On disconnect
(`plen<0`), calls `DepthRecorder_LogGap` with `reason="disconnect"`.

Also adds `cfg.record_depth` (int, default 0) with parser support.
main.cpp instantiates `DepthRecorder` and wires the pointer when
both `record_depth` and `depth_enabled` are set.

### c6 (this commit) — Settings panel + engine.cfg + CLAUDE.md

UI exposure for `record_depth` in Settings panel ("Tick Recording"
section, with a tooltip describing format + disk usage). engine.cfg
gets a documented entry. CLAUDE.md "Engine subsystem state" updated:
depth WS is no longer documented as "WORKING" without qualification —
now correctly notes that pre-Phase-8a it existed but didn't run.

Test binary `tests/depth_recorder_test.cpp` (17 assertions per
sidecar) lands in c7 — split out because it's substantial and
independent of the production code that already shipped in c2-c5.

## Plan amendments applied

Per cross-plan analysis 2026-04-25 evening, before any code:

1. Gap-detection logic placement: subplan put it in the thread,
   sidecar tested it in the recorder. Resolved by moving into recorder
   (amendment #1 in `plans/phase8a-depth-recorder.md`).
2. Gap algorithm: original "+1" check would false-positive every
   message. Corrected to backward-jump OR wallclock-gap OR explicit
   disconnect (amendment #2; per master plan errata).
3. Master plan errata's "add `last_seen_wallclock_us` to
   `DepthSharedState`" was superseded — recorder owns gap state now,
   not the shared state.

## Known limitations

- **Top-of-book only** — full L5 (10 levels) is a CSV format
  extension. Add when needed.
- **No replay path yet** — Phase 8a only records. Backtest replay
  of depth events is a separate work item, deferred until enough
  recordings exist to be worth replaying (≈ 2 weeks of data).
- **No depth-derived ML features yet** — also deferred; would
  invalidate every saved model bundle (anti-drift: ModelFeatures_Pack
  is UNCHANGED rule).
- **Crash-window gap is not marked** — recorder state in memory only.
  Restart resets `last_seen_id` to 0, first post-restart write skips
  the internal gap check. Real gaps from a crash window go silent.
- **User-data WS still not started** — same wired-but-not-started
  pattern as pre-Phase-8a depth. Phase 8 (maker/taker) probably
  needs it for executionReport parsing; will activate then.

## Anti-drift verified

Every commit in c1-c6:
- `ML_Headers/ModelInference.hpp::ModelFeatures_Pack` unchanged
- `ML_Headers/RollingStats.hpp::RollingStats_Push` unchanged
- `CoreFrameworks/ExecutionCore.hpp::ExecutionCore_Tick` unchanged
- `FEAT_*` constants unchanged
- `controller_test` 296/296 (post-Phase-5d baseline)
- All 4 targets build clean (engine, engine_gui, foxml_suite,
  controller_test)

## Tags

`phase8a-c1` … `phase8a-c6` mark each commit. `phase8a-complete`
will tag at the end of c7 (test binary).
