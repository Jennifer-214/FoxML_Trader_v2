# Failed Optimizations

Attempted optimizations that made things worse. Documented so we don't repeat them.

## 2026-03-27: PortfolioController struct reorder (REVERTED)

**Commit:** `post-struct-reorder`
**Rollback to:** `pre-struct-reorder`

### What we tried
Moved `buy_conds` from offset ~2344 (after the 2312-byte Portfolio) to offset 0 in PortfolioController. Theory: BuyGate reads buy_conds every tick, putting it at cache line 0 avoids eviction from ExitGate touching different cache lines.

### What happened
BuyGate: 53ns → 76ns (+43%)
PCTick: 925ns → 1068ns (+15%)
Hot p50: 1019ns → 2046ns (+100%)

### Why it was worse

**1. False sharing between writers and readers in cache line 1**

The reorder packed `tick_count` (WRITTEN every tick), `total_ticks` (WRITTEN every tick), and `portfolio.active_bitmap` (READ every tick by ExitGate) into cache line 1 (offsets 64-95). Every `tick_count++` invalidates the cache line that ExitGate needs to read, forcing a reload from L2/L3.

Old layout had these 36 cache lines apart (CL 0 vs CL 36). No false sharing.

**2. time(NULL) syscall NOT actually eliminated**

The time gate implementation still called `time(NULL)` on every tick inside the `if (!tick_gate)` block to check the time floor. The syscall was moved, not removed. Net effect: same ~500ns cost, plus more complex control flow.

### Lesson
- Packing hot fields together is WRONG when some are read-heavy and others are write-heavy
- Readers and writers must be in DIFFERENT cache lines to avoid invalidation
- The old "accidental" layout (Portfolio first, scalars after) was actually better because the 2312-byte Portfolio acted as a natural cache line separator between the read zone (bitmap + positions) and the write zone (tick_count, prev_bitmap)
- Always verify syscall elimination by checking the actual code path, not just the intended logic

### Correct approach (not yet implemented)
Separate into 3 zones with cache line padding between them:
```
Zone A (READ every tick):  buy_conds                    CL 0
  --- 64-byte padding to force new cache line ---
Zone B (READ every tick):  portfolio (bitmap + positions) CL 2+
  --- natural separation (Portfolio is 2312 bytes) ---
Zone C (WRITE every tick): prev_bitmap, tick_count, total_ticks  CL 38+
```

For time(NULL): either remove the time floor entirely (accept that low-volume periods may delay slow path), or check time() only every 16 ticks using `(tick_count & 0xF) == 0`.

## 2026-03-27: RollingStats cached outputs before ring buffers (REVERTED)

**Same commit as above.**

### What we tried
Moved cached outputs (price_avg, price_slope, etc.) from offset ~6664 to offset 0. Theory: strategies read these every slow tick, putting them at the start avoids chasing 104 cache lines into the struct.

### Why it was reverted
Reverted as part of the controller reorder rollback. The RollingStats change alone may have been neutral or positive — it was coupled with the controller change so we can't isolate the impact. Could re-test independently.
