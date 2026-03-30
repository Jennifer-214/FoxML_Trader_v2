#!/usr/bin/env python3
"""
foxml chart — standalone sidecar for tick_trader engine
connects to Binance websocket for live candles, reads trade CSV for markers
zero engine impact — completely separate process

usage: .chart-venv/bin/python tools/chart.py [--symbol btcusdt] [--csv path]
"""

import argparse
import json
import os
import threading
import time
from collections import deque
from datetime import datetime, timezone

import matplotlib
matplotlib.use('TkAgg')
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import matplotlib.ticker as mticker
from matplotlib.patches import FancyArrowPatch
import numpy as np
import pandas as pd
import websocket

# ═══════════════════════════════════════════════════════════════════════════════
# FOXML PALETTE
# ═══════════════════════════════════════════════════════════════════════════════
# FoxML Classic palette — matches ~/THEME/FoxML/themes/FoxML_Classic/palette.sh
C = {
    'bg':      '#1a1214',    # BG
    'bg_dark': '#150f0f',    # BG_DARK
    'panel':   '#2d1f27',    # BG_HIGHLIGHT
    'grid':    '#2d1a2d',    # BG_ALT
    'text':    '#d5c4b0',    # FG
    'fg_dim':  '#7a7a7a',    # FG_DIM
    'wheat':   '#d4b483',    # WHEAT
    'clay':    '#b0603a',    # CLAY
    'sand':    '#a89a7a',    # SAND
    'warm':    '#b0a498',    # WARM
    'primary': '#d4985a',    # PRIMARY
    'secondary':'#b8967a',   # SECONDARY
    'accent':  '#8a9a7a',    # ACCENT
    'green':   '#6b9a7a',    # GREEN
    'green_b': '#7aab88',    # GREEN_BRIGHT / OK
    'red':     '#b05555',    # RED
    'red_b':   '#c06868',    # RED_BRIGHT
    'yellow':  '#c4b48a',    # YELLOW
    'blue':    '#7a9ab4',    # BLUE
    'cyan':    '#7a9aab',    # CYAN
    'surface': '#3a414b',    # SURFACE
    'select':  '#4d2f34',    # SELECTION
    'comment': '#5a6270',    # COMMENT
    'vwap':    '#d4b483',    # WHEAT (dashed overlay)
}

# ═══════════════════════════════════════════════════════════════════════════════
# CANDLE ACCUMULATOR
# ═══════════════════════════════════════════════════════════════════════════════
class CandleAccumulator:
    def __init__(self, interval_sec=60, max_candles=120):
        self.interval = interval_sec
        self.max_candles = max_candles
        self.candles = deque(maxlen=max_candles)
        self.current = None  # {open, high, low, close, volume, buy_vol, sell_vol, time}
        self.vwap_pv = 0.0
        self.vwap_vol = 0.0
        self.lock = threading.Lock()

    def _bucket(self, ts):
        return int(ts // self.interval) * self.interval

    def push(self, price, volume, is_seller, timestamp_ms):
        ts = timestamp_ms / 1000.0
        bucket = self._bucket(ts)

        with self.lock:
            if self.current is None or bucket > self._bucket(self.current['time']):
                if self.current is not None:
                    self.candles.append(self.current)
                self.current = {
                    'time': ts,
                    'open': price, 'high': price, 'low': price, 'close': price,
                    'volume': 0.0, 'buy_vol': 0.0, 'sell_vol': 0.0,
                }

            c = self.current
            c['close'] = price
            c['high'] = max(c['high'], price)
            c['low'] = min(c['low'], price)
            c['volume'] += volume
            if is_seller:
                c['sell_vol'] += volume
            else:
                c['buy_vol'] += volume

            self.vwap_pv += price * volume
            self.vwap_vol += volume

    def snapshot(self):
        with self.lock:
            result = list(self.candles)
            if self.current is not None:
                result.append(self.current)
            vwap = self.vwap_pv / self.vwap_vol if self.vwap_vol > 0 else 0.0
            return result, vwap


# ═══════════════════════════════════════════════════════════════════════════════
# TRADE CSV READER
# ═══════════════════════════════════════════════════════════════════════════════
class TradeReader:
    def __init__(self, csv_path):
        self.csv_path = csv_path
        self.last_size = 0
        self.buys = []   # [(price, tick)]
        self.sells = []  # [(price, tick, reason)]
        self.equity = [] # [(tick, cumulative_pnl)]
        self.active_positions = []  # [(entry_price, tp, sl)]
        self.cumulative_pnl = 0.0

    def refresh(self):
        if not os.path.exists(self.csv_path):
            return
        size = os.path.getsize(self.csv_path)
        if size == self.last_size:
            return
        self.last_size = size

        self.buys.clear()
        self.sells.clear()
        self.equity.clear()
        self.cumulative_pnl = 0.0
        open_positions = []  # [(entry_price, tp, sl)]

        try:
            df = pd.read_csv(self.csv_path, on_bad_lines='skip')
            for _, row in df.iterrows():
                tick = row.get('tick', 0)
                price = row.get('price', 0)
                side = str(row.get('side', ''))

                if side == 'BUY':
                    tp = float(row.get('take_profit', 0) or 0)
                    sl = float(row.get('stop_loss', 0) or 0)
                    open_positions.append((float(price), tp, sl))
                    self.buys.append((float(price), int(tick)))
                elif side == 'SELL':
                    reason = str(row.get('exit_reason', ''))
                    entry = float(row.get('entry_price', price) or price)
                    qty = float(row.get('quantity', 0) or 0)
                    pnl = (float(price) - entry) * qty
                    self.cumulative_pnl += pnl
                    self.sells.append((float(price), int(tick), reason))
                    self.equity.append((int(tick), self.cumulative_pnl))
                    # remove matching open position
                    for i, (ep, _, _) in enumerate(open_positions):
                        if abs(ep - entry) < 0.01:
                            open_positions.pop(i)
                            break
        except Exception as e:
            print(f'[foxml chart] CSV parse error: {e}')

        self.active_positions = open_positions


# ═══════════════════════════════════════════════════════════════════════════════
# WEBSOCKET THREAD
# ═══════════════════════════════════════════════════════════════════════════════
def start_ws(symbol, accumulator):
    import ssl

    def on_open(ws):
        print(f'[foxml chart] websocket connected')

    def on_message(ws, msg):
        try:
            d = json.loads(msg)
            price = float(d['p'])
            volume = float(d['q'])
            is_seller = d['m']
            ts = int(d['T'])
            accumulator.push(price, volume, is_seller, ts)
        except (KeyError, ValueError) as e:
            print(f'[foxml chart] parse error: {e}')

    def on_error(ws, err):
        print(f'[foxml chart] ws error: {err}')

    def on_close(ws, code, msg):
        print(f'[foxml chart] ws closed ({code}), reconnecting...')
        time.sleep(3)
        run()

    def run():
        url = f"wss://data-stream.binance.vision:443/ws/{symbol}@trade"
        ws = websocket.WebSocketApp(url,
            on_open=on_open, on_message=on_message,
            on_error=on_error, on_close=on_close)
        ws.run_forever(sslopt={"cert_reqs": ssl.CERT_NONE})

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


# ═══════════════════════════════════════════════════════════════════════════════
# CHART RENDERER
# ═══════════════════════════════════════════════════════════════════════════════
class FoxmlChart:
    def __init__(self, accumulator, trade_reader):
        self.acc = accumulator
        self.trades = trade_reader

        plt.rcParams['toolbar'] = 'None'
        plt.rcParams.update({
            'figure.facecolor': C['bg'],
            'axes.facecolor': C['bg_dark'],
            'axes.edgecolor': C['surface'],
            'axes.labelcolor': C['sand'],
            'text.color': C['text'],
            'xtick.color': C['comment'],
            'ytick.color': C['comment'],
            'grid.color': C['grid'],
            'grid.alpha': 0.3,
            'font.family': 'monospace',
            'font.size': 9,
        })

        self.fig, (self.ax_price, self.ax_vol, self.ax_equity) = plt.subplots(
            3, 1, figsize=(14, 9),
            gridspec_kw={'height_ratios': [4, 1, 2], 'hspace': 0.08},
            sharex=False
        )
        # transparency handled by Hyprland compositor rule, not matplotlib
        self.fig.canvas.manager.set_window_title('foxml chart')

        # title
        self.fig.text(0.02, 0.97, 'foxml trader', fontsize=12,
                      color=C['primary'], fontweight='bold', va='top')
        self.price_text = self.fig.text(0.18, 0.97, '', fontsize=12,
                                         color=C['wheat'], fontweight='bold', va='top')

    def render(self):
        candles, vwap = self.acc.snapshot()
        self.trades.refresh()

        if len(candles) < 2:
            return

        self.ax_price.clear()
        self.ax_vol.clear()
        self.ax_equity.clear()

        # build arrays
        times = [datetime.fromtimestamp(c['time'], tz=timezone.utc) for c in candles]
        opens = [c['open'] for c in candles]
        highs = [c['high'] for c in candles]
        lows = [c['low'] for c in candles]
        closes = [c['close'] for c in candles]
        volumes = [c['volume'] for c in candles]
        buy_vols = [c['buy_vol'] for c in candles]

        n = len(candles)
        xs = np.arange(n)
        width = 0.6

        # ── CANDLESTICK CHART ──
        for i in range(n):
            color = C['green_b'] if closes[i] >= opens[i] else C['red']
            wick_color = C['sand'] if closes[i] >= opens[i] else C['comment']

            # wick
            self.ax_price.plot([xs[i], xs[i]], [lows[i], highs[i]],
                              color=wick_color, linewidth=0.8)
            # body
            body_low = min(opens[i], closes[i])
            body_h = abs(closes[i] - opens[i])
            if body_h < 0.01:
                body_h = 0.01
            self.ax_price.bar(xs[i], body_h, bottom=body_low, width=width,
                             color=color, edgecolor=color, linewidth=0.5)

        # VWAP overlay
        if vwap > 0:
            self.ax_price.axhline(y=vwap, color=C['vwap'], linewidth=1,
                                  linestyle='--', alpha=0.5, label=f'VWAP ${vwap:,.2f}')

        # rolling average
        if n > 10:
            window = min(20, n)
            avg = pd.Series(closes).rolling(window).mean()
            self.ax_price.plot(xs, avg, color=C['secondary'], linewidth=1, alpha=0.7)

        # entry/exit markers — map trades to nearest candle by price proximity
        last_price = closes[-1] if closes else 0
        time_range = (times[-1].timestamp() - times[0].timestamp()) if len(times) > 1 else 1

        for buy_price, buy_tick in self.trades.buys:
            # find nearest candle by price
            best_i = min(range(n), key=lambda i: abs(closes[i] - buy_price))
            self.ax_price.scatter(xs[best_i], buy_price, marker='^', s=80,
                                color=C['green_b'], edgecolors=C['text'], linewidths=0.5,
                                zorder=10)

        for sell_price, sell_tick, reason in self.trades.sells:
            best_i = min(range(n), key=lambda i: abs(closes[i] - sell_price))
            color = C['green_b'] if reason == 'TP' else C['red_b']
            self.ax_price.scatter(xs[best_i], sell_price, marker='v', s=80,
                                color=color, edgecolors=C['text'], linewidths=0.5,
                                zorder=10)

        # active position lines: entry (wheat), TP (green dashed), SL (red dashed)
        # numbered markers to differentiate which TP/SL belongs to which entry
        pos_styles = ['-', '-.', ':', (0, (5, 1))]  # different line styles per position
        for pi, (entry_price, tp, sl) in enumerate(self.trades.active_positions):
            style = pos_styles[pi % len(pos_styles)]
            tag = f'#{pi+1}'
            if entry_price > 0:
                self.ax_price.axhline(y=entry_price, color=C['wheat'], linewidth=1,
                                      linestyle=style, alpha=0.7)
                self.ax_price.text(n - 0.3, entry_price, f' {tag} ${entry_price:,.0f}',
                                   fontsize=7, color=C['wheat'], va='bottom')
            if tp > 0:
                self.ax_price.axhline(y=tp, color=C['green_b'], linewidth=0.8,
                                      linestyle=style, alpha=0.5)
                self.ax_price.text(n - 0.3, tp, f' {tag} TP',
                                   fontsize=7, color=C['green_b'], va='bottom')
            if sl > 0:
                self.ax_price.axhline(y=sl, color=C['red'], linewidth=0.8,
                                      linestyle=style, alpha=0.5)
                self.ax_price.text(n - 0.3, sl, f' {tag} SL',
                                   fontsize=7, color=C['red'], va='bottom')

        self.ax_price.set_ylabel('Price ($)', color=C['text'])
        self.ax_price.yaxis.set_major_formatter(mticker.FuncFormatter(lambda x, p: f'${x:,.0f}'))
        self.ax_price.grid(True, alpha=0.3)
        self.ax_price.set_xlim(-0.5, n - 0.5)

        # x labels as time
        tick_step = max(1, n // 8)
        self.ax_price.set_xticks(xs[::tick_step])
        self.ax_price.set_xticklabels([t.strftime('%H:%M') for t in times[::tick_step]])

        # update title
        self.price_text.set_text(f'BTCUSDT  ${last_price:,.2f}')

        # ── VOLUME BARS ──
        for i in range(n):
            ratio = buy_vols[i] / volumes[i] if volumes[i] > 0 else 0.5
            color = C['green'] if ratio > 0.5 else C['red_b']
            self.ax_vol.bar(xs[i], volumes[i], width=width, color=color, alpha=0.7)

        self.ax_vol.set_ylabel('Vol', color=C['text'])
        self.ax_vol.grid(True, alpha=0.3)
        self.ax_vol.set_xlim(-0.5, n - 0.5)
        self.ax_vol.set_xticks(xs[::tick_step])
        self.ax_vol.set_xticklabels([t.strftime('%H:%M') for t in times[::tick_step]])

        # ── EQUITY CURVE ──
        if self.trades.equity:
            eq_ticks = [e[0] for e in self.trades.equity]
            eq_vals = [e[1] for e in self.trades.equity]
            eq_xs = np.arange(len(eq_vals))

            self.ax_equity.plot(eq_xs, eq_vals, color=C['primary'], linewidth=1.5)
            self.ax_equity.axhline(y=0, color=C['surface'], linewidth=1, linestyle='-')

            # fill green above 0, red below
            eq_arr = np.array(eq_vals)
            self.ax_equity.fill_between(eq_xs, eq_arr, 0,
                                        where=eq_arr >= 0, color=C['green'], alpha=0.15)
            self.ax_equity.fill_between(eq_xs, eq_arr, 0,
                                        where=eq_arr < 0, color=C['red'], alpha=0.15)

            # trade markers
            for i, (tick, pnl) in enumerate(self.trades.equity):
                if i < len(self.trades.sells):
                    reason = self.trades.sells[i][2]
                    color = C['green_b'] if reason == 'TP' else C['red_b']
                    self.ax_equity.scatter(eq_xs[i], pnl, s=30, color=color,
                                          edgecolors=C['text'], linewidths=0.3, zorder=10)

            self.ax_equity.set_ylabel('P&L ($)', color=C['text'])
            self.ax_equity.yaxis.set_major_formatter(mticker.FuncFormatter(lambda x, p: f'${x:+,.2f}'))
        else:
            self.ax_equity.text(0.5, 0.5, 'waiting for trades...',
                               transform=self.ax_equity.transAxes,
                               ha='center', va='center', color=C['comment'], fontsize=11)

        self.ax_equity.grid(True, alpha=0.3)

        self.fig.canvas.draw_idle()
        self.fig.canvas.flush_events()


# ═══════════════════════════════════════════════════════════════════════════════
# MAIN
# ═══════════════════════════════════════════════════════════════════════════════
def main():
    parser = argparse.ArgumentParser(description='foxml chart')
    parser.add_argument('--symbol', default='btcusdt', help='trading pair')
    parser.add_argument('--csv', default=None, help='trade CSV path')
    parser.add_argument('--interval', type=int, default=60, help='candle interval (seconds)')
    args = parser.parse_args()

    # find trade CSV
    csv_path = args.csv
    if csv_path is None:
        csv_path = os.path.join('build', f'{args.symbol}_order_history.csv')
        if not os.path.exists(csv_path):
            csv_path = f'{args.symbol}_order_history.csv'

    acc = CandleAccumulator(interval_sec=args.interval)
    trades = TradeReader(csv_path)
    chart = FoxmlChart(acc, trades)

    # start websocket
    start_ws(args.symbol, acc)

    print(f'[foxml chart] connecting to {args.symbol}@trade ...')
    print(f'[foxml chart] reading trades from {csv_path}')
    print(f'[foxml chart] candle interval: {args.interval}s')

    plt.ion()
    plt.show()

    try:
        while True:
            chart.render()
            plt.pause(1.0)
    except KeyboardInterrupt:
        print('\n[foxml chart] shutting down')
    except Exception as e:
        print(f'[foxml chart] error: {e}')
    finally:
        plt.close('all')


if __name__ == '__main__':
    main()
