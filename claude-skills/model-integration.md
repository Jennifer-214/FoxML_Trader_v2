# Model Integration (Future)

How to connect FoxML ML models to the tick_trader execution engine.

## Architecture

```
FoxML (Python)                    tick_trader (C++)
┌─────────────────┐              ┌──────────────────┐
│ Data Pipeline    │              │ BinanceStream    │
│ Feature Eng      │              │ (raw ticks)      │
│ Model Inference  │──IPC/TCP──→│ ModelStrategy     │
│ Horizon Blend    │  conditions │ (reads buy_conds) │
│ Cost Arbitration │              │ BuyGate          │
│ Position Sizing  │              │ ExitGate         │
└─────────────────┘              └──────────────────┘
```

## Integration Points

The tick_trader architecture already supports this:

1. **BuyGate is condition-agnostic** — reads `buy_conds.price` and `buy_conds.volume`
   without knowing what set them. A model-driven strategy just sets these differently.

2. **Strategy interface** — add a `ModelDrivenStrategy` that reads conditions from
   a shared memory segment, Unix socket, or TCP connection instead of computing them
   from rolling stats.

3. **gate_direction** — model can signal buy-above or buy-below per tick.

## Proposed Strategy: ModelDriven

```cpp
template <unsigned F> struct ModelDrivenState {
    int socket_fd;              // connection to Python model server
    FPN<F> last_price_target;   // latest model-predicted entry price
    FPN<F> last_volume_target;  // latest model-predicted volume threshold
    int last_direction;         // 0 = buy below, 1 = buy above
    uint64_t last_update_tick;  // when conditions were last received
    int stale_threshold;        // max ticks before conditions considered stale
};
```

## FoxML Components That Map to tick_trader

| FoxML Component | tick_trader Equivalent |
|---|---|
| Prediction layer | Strategy BuySignal output |
| Barrier gating (P(peak) > 0.6) | Regime detector (VOLATILE = pause) |
| Cost arbitration | Entry spacing + fee floor |
| Position sizing | risk_pct * balance / price |
| Horizon blending | Multi-timeframe gate (rolling + rolling_long) |

## Prerequisites

1. tick_trader regime switching working and paper-traded
2. FoxML execution engine running with live data
3. IPC mechanism chosen (Unix socket recommended for same-machine)
4. Condition format defined (price, volume, direction, confidence)
