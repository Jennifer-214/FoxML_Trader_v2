
● Now trader and traderc just run the engine directly. If you want the CPU pinning + RT priority, wrap it
  yourself like before:

  sudo systemd-inhibit --what=idle:sleep:handle-lid-switch --who="fox_ml" --why="trader running" --mode=block
  taskset -c 1 chrt -f 99 ~/tick_trader_private/run.sh

────────────────────────────────────────────────────────────────────────────────────────────────────────────────
❯ 

