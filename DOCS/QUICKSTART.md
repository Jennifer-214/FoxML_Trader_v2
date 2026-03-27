# Quickstart

## Build

```bash
# engine binary (requires OpenSSL)
g++ -std=c++17 -O2 -lssl -lcrypto -o engine main.cpp

# tests (no network needed)
g++ -std=c++17 -O2 -I.. -o tests/controller_test tests/controller_test.cpp
tests/controller_test

# live data integration test (requires internet)
g++ -std=c++17 -O2 -lssl -lcrypto -o tests/binance_test tests/binance_test.cpp
tests/binance_test
```

## Run

```bash
./engine                    # uses engine.cfg in current directory
./engine /path/to/config    # custom config path
```

The engine connects to Binance, runs a warmup phase (observes market), then begins paper trading. Press `q` to quit cleanly.

## TUI Controls

| Key | Action |
|---|---|
| `q` | Quit (saves snapshot, closes positions, prints summary) |
| `p` | Pause/unpause (disables buy gate, exit gate keeps running) |
| `r` | Reload config from file (hot-swap tunable parameters) |

## First Run

1. Edit `engine.cfg` - set your symbol, starting balance, and parameters
2. Run `./engine`
3. Watch the warmup phase complete (builds market structure from live data)
4. The engine will start paper trading when conditions are met
5. Press `q` to stop - positions are saved to `portfolio.snapshot`
6. Restart `./engine` - it resumes from the snapshot with all state intact

## Files Created at Runtime

| File | Purpose |
|---|---|
| `btcusdt_order_history.csv` | Trade log (every buy and sell with prices, TP/SL, P&L) |
| `portfolio.snapshot` | Binary snapshot of portfolio state (survives restarts) |

## Resetting Paper Trading

```bash
rm portfolio.snapshot btcusdt_order_history.csv
./engine
```

Starts fresh with the `starting_balance` from config.

## Disconnect Handling

The engine handles disconnects gracefully:

- **WiFi drops / laptop sleep** - snapshot saved, reconnect on wake, positions preserved
- **Process crash** - last snapshot from most recent slow-path cycle is on disk, restart resumes
- **`q` to quit** - snapshot saved with positions, restart picks them up
- **24-hour Binance cutoff** - planned reconnect, positions force-closed (by design)

Positions that would have hit TP/SL while offline exit on the first tick after reconnect. This means gap risk — BTC could have moved significantly while you were disconnected.

## Running 24/7

The engine does NOT run while the laptop is asleep. For continuous operation:

```bash
# on a VPS or always-on machine
ssh your-server
tmux new -s trader
./engine engine.cfg    # with tui_enabled=0 for headless
# Ctrl+B, D to detach - engine keeps running
# tmux attach -t trader to reconnect later
```

Options: VPS ($5-10/month), home server, Raspberry Pi, or any machine that stays on.

## Headless Mode

Set `tui_enabled=0` in config to run without terminal output. The engine runs identically - useful for tmux/screen sessions and VPS deployment.

## Tuning

While the engine is running, edit `engine.cfg` and press `r` to reload. Hot-swappable parameters take effect immediately:

- `entry_offset_pct` - how far below rolling average to set buy gate
- `volume_multiplier` - how much larger than average a trade needs to be
- `take_profit_pct` / `stop_loss_pct` - exit conditions for new positions
- `spacing_multiplier` - minimum distance between entry prices
- All adaptive filter clamps (`offset_min`, `offset_max`, etc.)

See `DOCS/CONFIGURATION.md` for the full parameter reference.
