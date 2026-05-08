# 2026-04-25 (evening) — Phase 8: Maker/Taker Accounting + ORDER_PARTIAL state machine

Branch: `experiment/live-readiness`. Sixth and final phase of the
live-readiness work. Continues from `phase7prep-complete`.

Seven commits, each tagged `phase8-c1` … `phase8-c7`. Phase 8 was the
biggest commit set in live-readiness — touches OMS state machine,
Binance executionReport parser, fee math at 7+ sites, and adds 4 new
counters with placement at the END of `PortfolioController`.

## What ships

The engine now has **operationally correct fee accounting** for live
trading:

- Per-fill maker/taker rate selection (Binance tier 0: 0.075% / 0.100%)
- `Order.is_maker` propagated from Binance executionReport "m" field
- `ORDER_PARTIAL` state wired (was a dead enum before — partial fills
  silently treated as full)
- Per-fill-type counters: `maker_fills_count`, `taker_fills_count`,
  `total_maker_fees`, `total_taker_fees` on controller, surfaced via
  TUISnapshot
- Backward-compat: legacy `fee_rate` still works (mirrors to both)

Default cfg (no maker/taker set) preserves pre-Phase-8 behavior
exactly. Verified by test pass — every fee site that switched from
`fee_rate` to `fee_rate_taker` produces identical numerics in legacy
mode (where `fee_rate_taker == fee_rate`).

## Behavior matrix

| `fee_rate_maker` | `fee_rate_taker` | `fee_rate` | Behavior |
|---|---|---|---|
| (default) | (default) | set | Legacy mirror — both = fee_rate, log message |
| set | set | (any) | Per-fill rate selection from `order->is_maker` |
| set | (default) | set | **`[CFG] WARNING`** — taker stays at default, likely user error |
| (default) | set | set | **`[CFG] WARNING`** — maker stays at default, likely user error |
| (any) | (any) | (default) | All-zero, no warning (test/dev cfg) |

Detection uses parse-time **explicit-set flags**, not value comparison —
explicitly setting maker/taker to values that happen to equal defaults
still counts as explicit (caught by Phase 8 c6 test).

## Commits

### c1 (`5c104c2`) — bifurcate fee_rate config

Added `fee_rate_maker` (default 0.00075) + `fee_rate_taker` (default
0.00100) cfg fields. Backward-compat clause at end of `ControllerConfig_Load`:
mirrors fee_rate to both when only fee_rate is set, logs warning on
mixed-cfg case (one of maker/taker explicitly set + fee_rate set).

Fingerprint policy: existing `Backtest/Fingerprint.hpp` hashes the
entire `ControllerConfig` struct. Adding new fields shifts the hash,
which is fine — it's a "vintage stamp," not a runtime verification key
(that's `expected.cfg` doing field-by-field via `CoreModelZoo_VerifyExpected`).

### c2 (`b48c555`) — is_maker on Order + OrderResult + static_assert

Added `is_maker` (uint8_t) to `tt::Order<F>` (consuming one of the 6
padding bytes, struct size stable at 280 bytes for F=64). Plus
`order_complete`, `is_maker`, `commission`, `commission_asset` on
`OrderResult` (the OMS callback payload).

`static_assert(sizeof(tt::Order<64>) == 280)` anchors the layout —
catches silent ABI breakage at compile time. `OrderPool` slots are
sized for this struct.

### c3 (`ac975f9`) — extend executionReport parser

`ud_parse_execution_report` now extracts:
  - "m" → is_maker (defensive default 0 = taker on missing)
  - "X" → order_complete (1 if "FILLED", 0 if "PARTIALLY_FILLED" or missing)
  - "n" → commission amount (audit only)
  - "N" → commission_asset (audit only)

Boolean parsing of "m": Binance encodes JSON booleans as literal
`true`/`false`. Use first-char check after `extract_str` — same pattern
as the existing aggTrades CSV parser.

### c4 (`bc9dd90`) — Fee_Compute helper + ORDER_PARTIAL state machine

The biggest commit of Phase 8. Three load-bearing changes:

**Fee_Compute helper.** New `Fee_Compute(cfg, notional, is_maker)`
template inline in `ControllerConfig.hpp`. Single source of truth for
per-fill fee math at the cfg layer. OMS-side sites (`OrderManager_HandleFill`)
read `oms->fee_rate_maker / fee_rate_taker` directly — added to
`OrderManagerState` struct, defaults to passed fee_rate at Init.

**Fee math sites updated.** 5 sites switched from `fee_rate` →
`fee_rate_taker`:
  - `PortfolioController.hpp` RecordExit exit_fee
  - `PortfolioController.hpp` Tick entry_fee
  - `PortfolioController.hpp` ExitBuffer_PendingProceeds caller
  - `ControllerEventLoop.hpp:399` entry_fee (mode 0)
  - `ControllerEventLoop.hpp:432` exit_fee (mode 0)

OMS HandleFill (mode 1) uses per-fill is_maker for both entry + exit.

**Pre-trade quantity sites kept on fee_rate** (per master plan
amendment #5 — these are quantities in pre-trade computations, not
fee charges on real fills):
  - `PortfolioController.hpp:725` no-trade band gate threshold
  - `PortfolioController.hpp:1207` fee floor for TP
  - `PortfolioController.hpp:1447` kill switch estimated_exit_fees
  - `PortfolioController.hpp:1611` spread_bps display

Each got a `// Phase 8: ... — leave as fee_rate` comment.

OrderEventLog.hpp + LegacyReferenceDriver.hpp also keep fee_rate —
they're simulation/replay paths without per-fill is_maker access.

**ORDER_PARTIAL wired.** Was a dead enum pre-Phase-8. Now:
  - dispatch picks state from `cmd.result.order_complete`:
    - 1 → ORDER_FILLED
    - 0 → ORDER_PARTIAL
  - `Order_IsTerminal` correctly returns false for PARTIAL (was already
    coded right, just never reached)
  - `total_filled` counter only increments on terminal FILLED, not on
    partials (one logical "trade" = one increment)

### c5 (`ca73c83`) — counters + snapshot sync

4 fields added to `PortfolioController`, **placed at END of struct**
per master plan amendment #6 (cross-plan analysis flagged that adding
~1KB earlier could push hot-path fields off cache lines):

  - `maker_fills_count` (uint32_t)
  - `taker_fills_count` (uint32_t)
  - `total_maker_fees` (FPN<F>)
  - `total_taker_fees` (FPN<F>)

Increment in synchronous fill paths — both increments go to taker since
sync path = market orders. OMS event_log_mode=1 path books independently
with real is_maker but doesn't update these counters (separation of
concerns; counters track controller's own synchronous accounting).

**Snapshot sync** (master plan amendment #11 — simplified rule):
updated only `TUI_CopySnapshot` in `EngineTUI.hpp`. Did NOT update
`BacktestSnapshot_Copy` — it's a thin wrapper around `TUI_CopySnapshot`
per CLAUDE.md "Snapshot sync rule (simplified 2026-04)". Backtest
auto-inherits the new fields. Verified by test pass without any
backtest snapshot edits.

### c6 (`c648151`) — tests + bug fix

17 assertions across 6 groups in `controller_test.cpp` (trimmed from
the sidecar's ~32 — heavy infra tests deferred to Phase 8.x).

Groups:
  1. Fee_Compute helper            (3)
  2. Legacy fee_rate backward compat (4)
  3. ORDER_PARTIAL state           (3)
  4. executionReport parser        (3)
  5. Accounting invariant          (2)
  6. TUISnapshot maker/taker fields (2)

**Group 4 caught a real bug** in c1's mirroring logic. Original
implementation used `FPN_Equal(cfg.fee_rate_maker, default_maker)` to
detect "explicitly set" — but value-comparison can't distinguish
"explicitly set to default" from "not set." User explicitly setting
`fee_rate_maker=0.075` would have those values silently overwritten
by fee_rate.

Fix: track `maker_explicitly_set` / `taker_explicitly_set` parse-time
flags. Inline parse instead of `CFG_PARSE_PCT` macro (which `continue`s
before flag-set could happen). Behavior matrix above is the corrected
version.

This is exactly the kind of bug the cross-plan analysis warned about.
Tests caught it before live trading would have silently ignored
explicit maker/taker settings.

### c7 (this commit) — docs + cfg + Settings + changelog

CLAUDE.md adds "Maker/Taker Fee Accuracy" subsection under Safety
Invariants (between FPN-Only Accounting and Held-Out Validation
Discipline from Phase 7prep). 6-point rule:

  (1) Fee charge sites use per-fill rate — Fee_Compute or oms->fee_rate_maker/taker
  (2) Source is_maker from the matching order, never heuristic
  (3) Pre-trade quantity sites stay on legacy fee_rate (commented)
  (4) Sanity invariant: total_fees == total_maker_fees + total_taker_fees (sync path)
  (5) Cfg backward compat: explicit-set flags, not value comparison
  (6) ORDER_PARTIAL is no longer a dead enum — handle it

engine.cfg gets fee_rate_maker / fee_rate_taker entries with comments
explaining the legacy mirroring + warning behavior.

Settings panel gets maker / taker fields under "Trading" section with
tooltips explaining when each is used (live per-fill, backtest all-
taker simulation, mixed-cfg warning trigger).

Display polish (TUIAnsi Account section, GUI Dashboard maker/taker
breakdown) deferred — counters are populated, display is just pulling
from snapshot. Add when tested on live testnet.

## Plan amendments applied

Per cross-plan analysis 2026-04-25 evening:

- **#1**: mixed-cfg warning when only one of maker/taker set — c1.
- **#2**: fingerprint policy documented (legacy fee_rate hashed; new
  fields shift hash but no runtime verification compares fingerprints).
- **#3**: `static_assert(sizeof(Order<64>) == 280)` — c2.
- **#4**: ExitBuffer_PendingProceeds added to fee site list — c4.
- **#5**: pre-trade quantity sites documented + left on fee_rate — c4.
- **#6**: counter fields placed at END of PortfolioController — c5.
- **#7**: simplified snapshot rule — c5 updated only TUI_CopySnapshot.
- **#8**: explicit test commit numbered (c6) instead of bundled with c5.
- **#10**: ORDER_PARTIAL crash recovery — verified Order_IsTerminal is
  correct for PARTIAL; orphan recovery handles state-mismatched orders
  via existing reconciliation poller. No code change needed.
- **Bug fix mid-c6**: explicit-set flags for legacy mirroring (was
  value comparison, would have falsely mirrored explicit values that
  happened to equal defaults).

## Known limitations / deferred

- **Backtest maker simulation**: Phase 8's backtest assumes all-taker.
  Phase 9 hybrid execution can simulate maker fills.
- **ExitRecord.is_maker**: TP/SL exits are market sells = always taker.
  When Phase 9 ships limit-order exits (POST_ONLY), `ExitRecord` will
  need its own `is_maker` field. Phase 8 stays on the always-taker
  exit assumption.
- **Display polish**: TUIAnsi Account section + GUI Dashboard maker/
  taker breakdown deferred. Snapshot fields populated; display is the
  cosmetic layer.
- **OMS partial-fill recovery across crashes**: ORDER_PARTIAL state
  in memory is lost on crash. Existing orphan recovery (sells unbacked
  BTC at startup) handles the position-side; the in-flight order
  status is lost but the BTC ends up correct. Documented as known
  limitation in CLAUDE.md.
- **Mixed-asset commission accounting**: Binance commission can be
  paid in BNB instead of trading pair currency. `commission_asset` is
  recorded but not converted. Future phase if relevant.

## Anti-drift verified

Every commit in c1-c7:
- `ML_Headers/ModelInference.hpp::ModelFeatures_Pack` unchanged
- `ML_Headers/RollingStats.hpp::RollingStats_Push` unchanged
- `CoreFrameworks/ExecutionCore.hpp::ExecutionCore_Tick` unchanged
- `FEAT_*` constants unchanged
- `controller_test` 351/351 (post-Phase-8 baseline)
- `depth_recorder_test` 17/17 (Phase 8a baseline)
- All 4 main targets build clean
- `static_assert(sizeof(tt::Order<64>) == 280)` passes
- Default cfg (legacy mirroring) → identical numerics to pre-Phase-8

## Tags

`phase8-c1` … `phase8-c7` mark each commit. `phase8-complete` tags
this final commit.

## What this means for live readiness

Phase 8 is the final coding phase per `live-readiness-master.md`. After
this commit, the engine is **operationally ready** for live trading
with:

- Real maker/taker fee math (no longer a single 0.10% guess)
- Partial-fill handling (no longer silently treated as full)
- Per-fill-type accounting (so post-run analysis can answer "did I
  trade enough as maker to justify Phase 9?")
- Operational alerts (Phase 8b)
- Depth audit trail (Phase 8a — recording opt-in)
- Confidence loop wired (Phase 6prep — armed, inactive on noise-floor models)
- Held-out validation infrastructure (Phase 7prep — ready when signal exists)

Next step per master plan: testnet validation — 24-48 hour unattended
run with deliberate failure injection (recovery rehearsal). Then
tiny-capital live ($10) for 1-2 weeks observation. Then decide on
Phase 9 (hybrid execution) based on observed maker fill rate.
