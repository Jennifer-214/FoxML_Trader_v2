# Operational Runbook — Live Trading

This is the doc you actually consult when something needs human attention
during a live or testnet run. Procedures only, no design rationale (that
lives in `plans/` and `DOCS/changelogs/`).

Keep this open in another window during testnet observation.

## 1. Pre-live checklist (settle before $10 deposit)

Per `plans/live-readiness-master.md` Step 7. Each of these is a gate —
don't proceed to mainnet with any item unchecked.

- [ ] Exchange chosen: **Binance.US** (`use_binance_us=1` in cfg)
- [ ] Pair locked: **BTCUSDT** (existing focus + 730 days of recorded data)
- [ ] Capital deposited: **$10** (rounding-error if you find a bug)
- [ ] API key permissions: **READ + SPOT TRADE only** (NEVER withdraw)
      Verify in Binance.US dashboard. Whitelist withdrawal address
      even if you never grant withdraw — defense in depth.
- [ ] Notification channel decided + tested:
      - dunst:    `notify-send 'Engine: %s' '%s'` (local desktop only)
      - Discord:  webhook URL in `notify_command` (works headless on VPS)
      - ntfy.sh:  free, phone-app push, simplest setup
- [ ] Run-duration before review settled (suggested: 24h testnet, 7d live)
- [ ] All hard-stop config keys verified:
      - `use_real_money=1` for live (or 0 for paper)
      - `kill_switch_enabled=1`
      - `kill_switch_daily_loss_pct` set
      - `kill_switch_drawdown_pct` set
      - `record_ticks=1` (you want the audit trail)
      - `record_depth=1` if you want depth audit (50MB/day)
      - `notify_enabled=1` and `notify_command` set if alerting

## 2. Recovery rehearsal — induce failures on testnet BEFORE going live

Per `plans/live-readiness-master.md` "Recovery rehearsal" section.
Don't just observe a happy 24h. Deliberately break things and confirm
the engine handles them. **All of these on testnet only.**

| Failure mode | How to induce | Expected behavior |
|---|---|---|
| WS disconnect mid-trade | `sudo iptables -A OUTPUT -p tcp --dport 443 -j DROP` for 30s, then `sudo iptables -F` | Reconnect within `reconnect_delay`; alert fires once (cooldown collapses storm); no duplicate fills; depth recorder gap marker appears in CSV |
| Crash mid-fill | `kill -9 $(pgrep engine_gui)` between submit and fill | Restart picks up via orphan recovery; in-flight order shows ORPHAN; engine reconciles or sells |
| Disk full | `dd if=/dev/zero of=/tmp/filler bs=1M count=10000` to fill /tmp during a recording run | DepthRecorder + TickRecorder log error + disable. Trading continues. NOTIFY_ALERT fires. |
| Kill switch trip | Set `kill_switch_daily_loss_pct=0.001` (0.1%), wait until tripped | Trigger fires, alert sent, all buying halts, exit gates continue working, recovery counter starts |
| API key revoked | Disable testnet key in Binance.US dashboard | Order submissions get 401, ORDER_REJECTED, alert fires (NOTIFY_CRITICAL), engine refuses to retry blindly |
| Clock skew | `sudo date -s "+10 minutes"` then back | Cooldown still works (uses CLOCK_MONOTONIC per Phase 8b); wallclock-based gap markers may have brief inconsistency that resolves |

**Do at least 3 of these before going live.** If any fail unexpectedly,
fix before progressing.

## 3. Alert response — what to do when the phone buzzes at 3am

| Alert subject | First action | Diagnosis |
|---|---|---|
| `Engine kill switch — daily loss` | DON'T touch the engine yet. Open `logging/engine.log`, grep for `[KILL]` to see the trigger context (price, equity, position dump). | Did the strategy get unlucky on a single bad day, or is there a systematic problem? Check session start time vs trip time — if early in the session, likely market-driven. If late, your strategy may not be working. |
| `Engine kill switch — drawdown` | Same as above. Drawdown is from peak, so even if today is a small loss, total drawdown can trigger. | Open the equity curve. Is drawdown from a single bad cluster of trades, or from sustained underperformance? |
| `Engine halted — orphan detection` | Critical. Engine refuses to reconnect with positions it can't reconcile. | Manual reconciliation: check Binance.US dashboard for current BTC + USDT balance. Does it match the engine's last logged state? If not, you have a real reconciliation problem. |
| `Orphan recovery at startup` | Informational. Engine is auto-selling stranded BTC. | Check the post-sweep balance log line. Was the recovered amount what you expected from your last known-good balance? |
| `Orphaned real positions detected` | The engine's tracked positions don't match its real BTC balance. Auto-selling. | Same diagnosis as `Engine halted — orphan detection`, but the engine made its own call to recover. Verify the post-sell balance. |
| `Binance trade WS disconnected` | Wait for reconnect. Cooldown collapses repeated alerts. | If it persists past reconnect_delay × 5, network or Binance issue. Check internet, then Binance status page. |
| `Binance depth WS disconnected` | Same as trade WS. book_imbalance gate reads stale data until reconnect. | Less urgent than trade WS; unless you have `min_book_imbalance > 0`, gate is inert. |
| `Binance user-data WS disconnected` | Order fills + balance updates blocked. Trading continues optimistically; will reconcile on reconnect. | Same diagnosis as trade WS. If sustained, kill the engine and investigate before more trades happen. |

**Default response if you're not sure**: stop trading (`kill -INT $(pgrep
engine_gui)`), wait until you understand. Capital loss from "stop and
think" is bounded; from "panic-trade" can be unbounded.

## 4. Position reconciliation

When engine state and exchange state disagree:

```bash
# 1. Capture current state
binance-cli wallet  # or Binance.US web dashboard
grep "[LIVE]" logging/engine.log | tail -20

# 2. Compare
# - Engine's tracked balance: last [LIVE] startup_balance line
# - Engine's tracked BTC qty: last "active_count > 0" snapshot
# - Exchange's actual balance: from binance-cli or web

# 3. Decide
# - Match: nothing to do, engine continues
# - BTC mismatch < 0.0001 (dust): ignore, will resolve on next sell
# - BTC mismatch ≥ 0.0001: shut down engine, manually sell to flat,
#   restart with clean snapshot (delete snapshot.bin)
# - USDT mismatch: usually means a fee was paid the engine didn't track —
#   review the trade log CSV for the discrepancy window
```

## 5. Restart procedure

```bash
# Clean shutdown (let positions close naturally if possible)
kill -INT $(pgrep engine_gui)
# Wait for "[ENGINE] cleanup complete" in logs

# If unresponsive:
kill -9 $(pgrep engine_gui)

# Verify exchange state is clean before restart
binance-cli wallet
# If BTC > 0 and you intended to be flat, sell manually OR let
# orphan recovery handle it on startup.

# Start
./build_gui/engine_gui >> logging/engine.log 2>&1 &
disown   # detach from terminal

# OR for unattended VPS run:
nohup ./build_gui/engine_gui >> logging/engine.log 2>&1 &
```

For systemd / supervisord setups, see `OPS/systemd/` (TBD — set up
when going to VPS).

## 6. Latency verification (one-shot, before going live)

Per `plans/live-readiness-master.md` "Latency regression check":
verify Phase 8's hot-path-adjacent change (Fee_Compute ternary in
warm path) didn't regress hot-path latency.

```bash
# Build the profiling variant
cmake -B build_lat -DLATENCY_PROFILING=ON
cmake --build build_lat -j$(nproc)

# Run for ~10 minutes against testnet (or recorded ticks if no live feed)
./build_lat/engine

# In TUI, observe the latency stats panel:
#   - hot path p50, p95, p99 (BuyGate + PositionExitGate execution time)
#   - slow path p50, p99 (PortfolioController_Tick slow-path block)
```

**Acceptance gate**: hot path p99 ≤ 1.1× pre-Phase-8 baseline.
Pre-Phase-8 baseline (from README): hot path p95 ~53ns, p99 ~936ns.

If p99 > 1030ns (1.1× of 936ns), investigate before going live:
- Check `Fee_Compute` is being inlined (it's `static inline template`)
- Check the `book_imbalance` ACQUIRE load isn't on the hot path (it's
  on every tick before slow path, but should be predicted)
- Profile with `perf record` if needed

## 7. What to grep in logs

```bash
# All kill switch events
grep -E "\[KILL\]" logging/engine.log

# All disconnect/reconnect events
grep -E "reconnect|disconnected" logging/engine.log

# All orphan-related events
grep -E "orphan" logging/engine.log

# Notifications fired
grep -E "\[NOTIFY" logging/engine.log

# Trade history (CSV)
tail -100 logging/BTCUSDT_order_history.csv

# Today's depth gap markers
grep "# GAP" data/BTCUSDT/depth/$(date +%Y-%m-%d).csv

# Today's orderbook recording size (sanity check)
ls -lah data/BTCUSDT/depth/$(date +%Y-%m-%d).csv
# expect ~50MB if depth_enabled=1 + record_depth=1 ran for 24h
```

## 8. Tax records / audit trail

Every fill is in `logging/{SYMBOL}_order_history.csv` (open this
quarterly for tax reporting):

```
timestamp,side,price,qty,fee,realized_pnl,strategy,reason
```

For a year-end summary, the `total_fees` field in TUISnapshot is the
running total. For maker/taker breakdown (Phase 8), `total_maker_fees`
+ `total_taker_fees` give the split — useful if your tax jurisdiction
treats them differently (some don't, but check).

## 9. When to stop the engine and call it

If after the testnet rehearsal + 1-2 weeks tiny-capital live, ANY of:

- Engine state and exchange state diverge silently
- Crashes that don't auto-recover
- Strategies underperform vanilla buy-and-hold by more than fees
- Maker fill rate is 0% across a 7-day window AND you intended to
  ship hybrid execution

...don't ramp capital. Diagnose, fix, re-validate from `plans/live-
readiness-master.md` Step 6.

## 10. Capital sizing discipline

Per CLAUDE.md (when added by Phase 6 ship):

- Testnet: any `core_N_risk_pct` (testnet money is fake; let it run hot)
- Tiny-capital live ($10-$2000): per-position should be ≥ $1 to be
  meaningful. With max_positions=1 and risk_pct=0.15, $10 capital → $1.50
  per position. OK for fee testing, useless for strategy validation.
  Consider $200-500 for actual strategy observation.
- Hard ceiling: don't run with `sum(core_N_risk_pct) > 0.50`. Going
  above 50% means a single bad day can wipe half. Set the ceiling
  intentionally.

## Appendix: tag rollback points

If anything goes catastrophically wrong, here are the clean save points:

- `live-readiness-coding-complete` — all 6 phases of live-readiness coding done
- `phase8-complete` — same point, different name
- `phase5d-merged` — pre-live-readiness, post-Phase-5
- `main-backup-2026-04-25` — undo the entire merge

`git reset --hard <tag>` from `experiment/live-readiness` recovers.
