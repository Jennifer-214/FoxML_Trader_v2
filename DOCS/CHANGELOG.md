# Changelog

## Version Summary

| Version | Date | Highlights |
|---------|------|------------|
| 3.0.27 | 2026-03-25 | Extract 10 hardcoded constants to engine.cfg (squeeze_decay, adapt scales, risk ratios), revert to sparkline charts |
| 3.0.26 | 2026-03-25 | **P&L accounting fix** — total P&L derived from equity, force-close/undo/orphan sell properly book balance+fees, configurable fee_floor_mult |
| 3.0.25 | 2026-03-25 | Disable trailing TP for mean reversion (fixed exits in ranging), fix paper trading undo guard |
| 3.0.24 | 2026-03-25 | PAPER/LIVE mode indicator, separate paper/live snapshots, dynamic POS display, buy gate volume+status |
| 3.0.23 | 2026-03-25 | Separate paper/live snapshots, POS display shows config max_positions, P&L chart colors by value sign, buy gate shows volume gate + live status indicator |
| 3.0.22 | 2026-03-25 | Half-block area charts for all graphs (price/P&L/vol), P&L color-by-sign fix |
| 3.0.21 | 2026-03-24 | **TP/SL audit fix** — correct 4:1 asymmetry in downtrend regime adjustment, SL floor enforcement, braille area charts, buy gate direction, auto-logging, wall-time slow-path floor |
| 3.0.20 | 2026-03-24 | Direction-aware regime detection — TRENDING_DOWN pauses buying in downtrends, prevents momentum dead cat bounce entries |
| 3.0.19 | 2026-03-24 | **CRITICAL:** Heap corruption fix — zero-alloc SSL (SSL_clear reuse), same-tick guard, balance sync guard, external sell detection |
| 3.0.18 | 2026-03-24 | Single-slot position management, sell-all-BTC, paper/live sync, startup BTC recovery |
| 3.0.17 | 2026-03-23 | Fix: connected gate blocked all live orders, REST close-after-request, signed query fix |
| 3.0.16 | 2026-03-23 | AGPL-3.0 copyright headers, REST reconnect fix, live TUI status improvements |
| 3.0.15 | 2026-03-23 | Live trading: fire-and-forget REST, live bitmap, balance sync, orphan detection, qty validation, 24h sell |
| 3.0.14 | 2026-03-23 | Slippage simulation, zero-pollution TUI (slow-path snapshot + live price), warmup trim, live trading plan |
| 3.0.13 | 2026-03-23 | Latency profiling overhaul: busy-poll, core isolation, native 128-bit, warmup visibility fix |
| 3.0.12 | 2026-03-22 | Always warmup after snapshot load, exit drain during warmup, revert TUI to 9-section layout |
| 3.0.11 | 2026-03-22 | TUI consolidation (9→6 sections), fox art fix, BUYING PAUSED wording, W/L in top bar |
| 3.0.10 | 2026-03-22 | Warmup waits for full short window (128 samples), version strings synced, dynamic warmup display |
| 3.0.9 | 2026-03-22 | Public release — README rewrite, screenshots, gitignore cleanup |
| 3.0.8 | 2026-03-22 | Fill rejection diagnostics in BuyGate section, right panel data tracking |
| 3.0.7 | 2026-03-22 | Warmup gates on slow-path sample count — prevents buy spam on restart |
| 3.0.6 | 2026-03-21 | Post-SL cooldown — pauses buying after stop loss to prevent falling knife entries |
| 3.0.5 | 2026-03-21 | Spike ratio bugfixes (init + stale data), momentum/regime test coverage (118 assertions) |
| 3.0.4 | 2026-03-21 | Volume spike detection, spacing relaxation on high-conviction dips |
| 3.0.3 | 2026-03-21 | Adaptive momentum TP/SL (R²-scaled + ROR acceleration bonus) |
| 3.0.2 | 2026-03-21 | Momentum TP/SL ×100 bugfix, diff-based rendering (zero flicker), adaptive position layout |
| 3.0.1 | 2026-03-21 | Flicker fix, uptime fix, kaomoji fox art, per-bar P&L sparkline, enriched positions, volume chart |
| 3.0.0 | 2026-03-21 | ANSI TUI (zero deps), regime signals display, score-based detection, RollingStats least-squares, snapshot v7 |
| 2.0.1 | 2026-03-21 | Chart label fixes, jitter fix, graph rendering fixes |
| 2.0.0 | 2026-03-21 | FTXUI TUI rewrite, canvas charts, preset layouts, CMake |
| 1.0.4 | 2026-03-21 | Entry spacing floor 0.01%→0.03%, docs refresh |
| 1.0.3 | 2026-03-21 | Wall clock durations, enhanced trade log CSV, metrics log |
| 1.0.2 | 2026-03-21 | Sequential position numbering, warmup=128 |
| 1.0.1 | 2026-03-21 | Bug fixes, shared functions, snapshot v5, Makefile |
| 1.0.0 | 2026-03-21 | Regime detection, momentum strategy, strategy dispatch |
| 0.9.1 | 2026-03-21 | Entry spacing floor fix |
| 0.9.0 | 2026-03-21 | Latency profiling, struct reorder, p50/p95 tracking |
| 0.8.1 | 2026-03-21 | TUI snapshot update rate fix |
| 0.8.0 | 2026-03-21 | Multicore TUI, buffered trade log, inline comparisons |
| 0.7.1 | 2026-03-21 | Time-based exit, BinanceConfig parser fix |
| 0.7.0 | 2026-03-20 | Trailing take-profit (SNR×R² gated) |
| 0.6.x | 2026-03-20 | Stddev offset, multi-timeframe gate, strategy library |
| 0.5.0 | 2026-03-20 | Strategy library architecture |
| 0.4.x | 2026-03-20 | Trading fees, position sizing, risk management, TUI theme |
| 0.3.0 | 2026-03-20 | Idle squeeze, paper balance, portfolio persistence |
| 0.2.0 | 2026-03-20 | Live Binance stream, TUI, rolling stats, adaptive filters |
| 0.1.0 | 2026-03-19 | Initial release, portfolio controller, 80 tests |

## Detailed Changelogs

- [2026-03-24 (d)](changelogs/2026-03-24-d.md) — v3.0.21 (TP/SL audit fix, braille charts, auto-logging, wall-time slow-path floor)
- [2026-03-24](changelogs/2026-03-24.md) — v3.0.18 (single-slot position management, paper/live sync, BTC dust elimination)
- [2026-03-22](changelogs/2026-03-22.md) — v3.0.7 through v3.0.12 (warmup fix, fill diagnostics, snapshot warmup, public release)
- [2026-03-21](changelogs/2026-03-21.md) — v0.7.1 through v3.0.6 (regime switching, latency profiling, ANSI TUI, volume spikes, SL cooldown)
- [2026-03-20](changelogs/2026-03-20.md) — v0.2.0 through v0.7.0 (live trading, strategies, risk management)
- [2026-03-19](changelogs/2026-03-19.md) — v0.1.0 (initial release)
