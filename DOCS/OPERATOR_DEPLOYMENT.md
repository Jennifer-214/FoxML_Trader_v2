# Operator Deployment Runbook

**v5.11.0a — system-level tuning for sub-microsecond determinism.**

The engine's source-side optimizations (FTZ/DAZ, mlockall, TCP_NODELAY,
PGO, branchless hot path, AVX-512 vectorization) are necessary but not
sufficient. Linux's defaults are tuned for general-purpose throughput,
not HFT tail latency. This runbook covers the OS-level config that
must accompany the engine binary to actually achieve the latency
profile the source code is engineered for.

**Audience:** operator deploying the engine to a production-class
Linux box (i7-11850H or similar; AVX-512-capable).
**Source audits:** `DOCS/LATENCY_OPTIMIZATION_AUDIT.md` Parts 6.3 + 13;
`DOCS/STRATEGY_AND_CODING_RULES.md` §8 + §10 + §11.

---

## Why this matters

User-space pinning (`pthread_setaffinity_np`) is insufficient. The
Linux kernel can still preempt your hot-path thread for:
- Timer interrupts (`HZ` ticks, ~1-10ms of jitter every few seconds)
- RCU callbacks (~hundreds of microseconds, sporadic)
- IRQ delivery from devices not on the hot-path core
- Page faults (closed by `mlockall` in v5.11.0.B, but only for memory
  the engine has already touched — `MCL_FUTURE` covers post-init too)
- C-state wakeups (~microseconds when the core was idle)
- P-state frequency ramp-up (~tens of microseconds from sleep)

Each of these causes a tail-latency spike that no amount of branchless
code can eliminate. The runbook below configures Linux to STAY OUT OF
THE WAY of the hot-path cores.

---

## Prerequisites

- Linux 5.x or later (tested on 6.x mainline + Arch).
- Root access (or sudo).
- AVX-512-capable CPU (i7-11850H, Xeon Gold/Platinum, recent EPYC, etc.)
  for v5.11.1+ vectorized hot path.
- At least 4 physical cores; recommended 8+ to leave headroom for
  housekeeping cores.

---

## Step 1 — Boot parameters (`/etc/default/grub` or systemd-boot)

These remove cores from kernel scheduling entirely. Edit your bootloader
config and add the following to the kernel cmdline. Then update the
bootloader (`grub-mkconfig` / `bootctl update`) and reboot.

### Example for a 12-core machine (cores 0-11)

Reserve cores 4-11 for the engine; leave 0-3 for the OS + housekeeping.

```
isolcpus=4-11 nohz_full=4-11 rcu_nocbs=4-11 \
intel_idle.max_cstate=0 processor.max_cstate=0 \
mce=off audit=0 \
nosoftlockup nowatchdog
```

**Per-flag:**

| Flag | Effect |
|---|---|
| `isolcpus=4-11` | Removes cores 4-11 from the kernel's general scheduler. Userspace code only runs there if explicitly pinned (the engine does this via `pthread_setaffinity_np`). |
| `nohz_full=4-11` | Stops the periodic timer tick on cores 4-11 when only one task is running on each (which the engine guarantees by design). Eliminates ~1ms tick jitter. |
| `rcu_nocbs=4-11` | Offloads RCU callback processing OFF the isolated cores, onto cores 0-3. Eliminates the RCU stall class. |
| `intel_idle.max_cstate=0` | Disables deep CPU sleep states. Cores stay at C0 (running) or C1 (auto-halt). C-state wakeup is ~µs that we don't want on hot path. |
| `processor.max_cstate=0` | Same intent for AMD/non-Intel. Belt-and-suspenders. |
| `mce=off` | Disables Machine Check Exception handling. Optional, only for embedded/dedicated boxes; leave on for production servers where you want hardware-fault visibility. |
| `audit=0` | Disables kernel audit subsystem. Removes a sporadic source of context switches. |
| `nosoftlockup nowatchdog` | Removes soft lockup detection threads (one less periodic interrupt source). |

After reboot verify:
```bash
cat /proc/cmdline                  # confirms the flags landed
cat /sys/devices/system/cpu/isolated  # should show "4-11"
cat /sys/devices/system/cpu/nohz_full # should show "4-11"
```

---

## Step 2 — CPU frequency governor

Set the `performance` governor on the isolated cores (or all cores).
Default `powersave` / `schedutil` ramps frequency based on demand,
introducing wakeup-from-idle stalls.

```bash
# One-shot (resets on reboot):
sudo cpupower frequency-set -g performance

# Persistent (Arch / systemd-based):
sudo systemctl enable --now cpupower.service
# Edit /etc/default/cpupower and set: governor='performance'
```

Verify:
```bash
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor   # should be "performance"
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq    # should be at base or higher
```

Disable Intel turbo if you want bytewise-deterministic latency
(turbo can throttle cores back unpredictably under thermal load):

```bash
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
```

(Trade-off: ~10% peak throughput loss for ~5x lower p99.99 jitter.
Worth it for engines whose objective is tail consistency.)

---

## Step 3 — SCHED_FIFO promotion (`chrt -f 99`)

User-space pinning + `isolcpus` is necessary but not sufficient — the
kernel still treats engine threads as `SCHED_OTHER` (CFS) by default,
which means OS housekeeping tasks can preempt them. Promote the
engine's hot-path threads to `SCHED_FIFO` priority 99.

### Recommended: launch via `chrt`

```bash
sudo chrt -f 99 /path/to/engine engine.cfg
```

`-f` = SCHED_FIFO. `99` = highest real-time priority. Engine inherits
this; spawned slow-path threads also run at SCHED_FIFO 99.

### Alternative: in-process call (engine-side change, NOT yet shipped)

If you want the engine to self-promote post-init, the canonical pattern
is `pthread_setschedparam(thread, SCHED_FIFO, &param)` per worker. This
isn't in the engine yet (would land in v5.11.X if needed); for now use
`chrt` at launch.

### Caveat: RLIMIT_RTPRIO

By default non-root users can't promote to SCHED_FIFO. Either:
- Run as root (simplest for dedicated boxes).
- Edit `/etc/security/limits.conf`:
  ```
  caramel    hard    rtprio    99
  caramel    soft    rtprio    99
  caramel    hard    memlock   unlimited
  caramel    soft    memlock   unlimited
  ```
- Or use systemd unit with `LimitRTPRIO=99` + `LimitMEMLOCK=infinity`.

---

## Step 4 — NIC interrupt affinity

The network card delivers IRQs to whichever cores it's configured to.
By default, NIC IRQs round-robin across all cores — including your
isolated cores, which is exactly what we want to prevent.

### Pin NIC IRQs to housekeeping cores (0-3)

```bash
# Find the NIC's IRQ numbers
grep eth0 /proc/interrupts | awk '{print $1}' | tr -d ':'

# For each IRQ, set CPU affinity mask to cores 0-3 (mask = 0x0f).
for irq in $(grep eth0 /proc/interrupts | awk -F: '{print $1}'); do
    echo 0f | sudo tee /proc/irq/$irq/smp_affinity
done
```

### Disable irqbalance (would re-spread IRQs)

```bash
sudo systemctl disable --now irqbalance
```

### Disable NIC interrupt coalescing (audit Part 13.4)

Default coalescing batches packets to reduce per-packet overhead at
the cost of latency (waits up to ~10µs to coalesce small packets).
For HFT, every µs counts:

```bash
sudo ethtool -C eth0 rx-usecs 0 rx-frames 0 tx-usecs 0 tx-frames 0
sudo ethtool -C eth0 adaptive-rx off adaptive-tx off
```

Verify:
```bash
ethtool -c eth0 | grep -E "rx-usecs:|adaptive-rx:"
# rx-usecs should be 0; adaptive-rx should be off
```

---

## Step 5 — NUMA locality

If the box has 2+ NUMA nodes (typical for 2-socket servers), the
engine's threads, memory allocations, and NIC interrupts must all be
on the same node. Cross-socket QPI/UPI traversal costs ~80-120ns per
cache line.

### Find the NUMA topology

```bash
numactl --hardware
# Lists nodes + which CPUs belong to each.

# Find which NUMA node the NIC is on:
cat /sys/class/net/eth0/device/numa_node
# 0 or 1; -1 means no NUMA preference (single-socket box)
```

### Bind engine to one node

If NIC is on NUMA node 1, bind everything there:

```bash
sudo chrt -f 99 numactl --cpunodebind=1 --membind=1 /path/to/engine engine.cfg
```

Verify post-launch:
```bash
cat /proc/<pid>/status | grep -E "Cpus_allowed_list|Mems_allowed_list"
```

---

## Step 6 — Huge pages

Default Linux uses 4KB pages. For the engine's working set (zoo + scaler
+ ring buffers + cfg = ~hundreds of MB), each page mapping consumes a
TLB entry. TLB misses cause an expensive page-table walk — ~hundreds
of ns per miss.

### Enable transparent huge pages globally

```bash
echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
echo always | sudo tee /sys/kernel/mm/transparent_hugepage/defrag
```

### Or: explicit 2MB huge pages for engine arena (v5.11.6 will use these)

```bash
# Reserve 1024 × 2MB pages = 2GB
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages

# Persistent in /etc/sysctl.d/40-hugepages.conf:
vm.nr_hugepages = 1024
```

The engine's `mlockall(MCL_CURRENT | MCL_FUTURE)` (v5.11.0.B) keeps
all pages resident; huge pages reduce the *number* of page mappings.
v5.11.6 will explicitly request `MAP_HUGETLB` on the unified arena
allocation; until then transparent huge pages cover the bulk
allocations.

---

## Step 7 — RLIMIT_MEMLOCK (for `mlockall`)

The engine's `mlockall` call (v5.11.0.B) requires `RLIMIT_MEMLOCK`
soft limit ≥ 256MB (the engine's typical resident size). Default user
limit is often 64KB which would cause `mlockall` to fail at boot
(engine logs "[v5.11.0.B] FATAL: mlockall failed: Cannot allocate
memory" and exits 1).

### Per-shell

```bash
ulimit -l unlimited
```

### Persistent (`/etc/security/limits.conf`)

```
caramel    hard    memlock    unlimited
caramel    soft    memlock    unlimited
```

### systemd unit

```ini
[Service]
LimitMEMLOCK=infinity
```

Verify post-launch:
```bash
cat /proc/<engine_pid>/status | grep VmLck
# Should show non-zero (the engine's locked memory size)
```

---

## Step 8 — Network bypass (future, NOT in scope for v5.11)

For sub-microsecond round-trip to a colocated exchange gateway, the
Linux kernel network stack itself becomes the bottleneck (~5-10µs
syscall overhead per `send()` / `recv()`). True HFT rigs use:

- **DPDK** — kernel-bypass userspace networking; engine talks directly
  to the NIC via UIO/VFIO mappings.
- **Solarflare EFVI / Onload** — vendor-specific bypass; ~1µs RTT
  improvements common.
- **AF_XDP** — Linux-native bypass via eBPF; lighter integration than
  DPDK.

Out of scope for v5.11; deferred to v5.13+ if operator is co-located.
For Binance retail (current target), the OS network stack is fine.

---

## Quick verification checklist

After deploying with all the above, confirm at runtime:

```bash
# Boot params landed:
cat /proc/cmdline | grep isolcpus

# Cores actually isolated:
cat /sys/devices/system/cpu/isolated

# CPU governor:
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor

# Engine PID + thread counts:
pgrep -a engine
ls /proc/$(pgrep engine)/task | wc -l

# Engine threads at SCHED_FIFO:
for tid in /proc/$(pgrep engine)/task/*/; do
    cat "${tid}sched" | grep -E "policy|prio"
done

# Engine memory locked:
cat /proc/$(pgrep engine)/status | grep -E "VmLck|VmRSS"

# NIC interrupts NOT on isolated cores:
cat /proc/interrupts | grep eth0
# CPU columns 4-11 should show 0 (or near-zero) interrupts

# Engine on the right NUMA node:
cat /proc/$(pgrep engine)/status | grep Cpus_allowed_list
```

If any of these don't match expectations, re-walk the relevant section
above before relying on the engine's latency numbers.

---

## Latency expectations after full deployment

With all 8 steps applied, the engine's per-tick hot path should sustain:
- **p50:** ~40-80ns (post-v5.11.1 AVX-512 ship)
- **p99:** ~200-400ns
- **p99.9:** ~1-3µs (mostly OS jitter floor; impossible to push lower
  without DPDK)
- **p99.99:** ~10-50µs (rare scheduler / IRQ events)

Without this runbook applied (default kernel, no isolcpus, no
SCHED_FIFO, default IRQ affinity):
- **p50:** ~40-80ns (CPU is the same)
- **p99:** ~5-50µs (kernel preemption visible at the 99th)
- **p99.9:** ~hundreds of µs (timer ticks + rcu callbacks)

The engine source-side optimizations only get you the p50 number. The
p99 / p99.9 wins come from THIS document.

---

## Thermal monitoring (added v5.11.2 context)

Sustained engine operation can push CPU thermals into throttling territory,
especially on laptop chassis (limited cooling). Thermal throttling drops
frequency mid-tick → catastrophic tail latency spikes (observed up to ~1ms
p99 on slow path during sustained load on a non-deployment laptop).

**Monitor during sustained operation:**
```bash
# Watch core temperatures (interval 1s):
watch -n1 'sensors | grep -E "Core|Tdie"'

# Watch CPU frequency (drops below base = throttling):
watch -n1 'cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq | head -8'

# Detect throttle events (intel_pstate cumulative count):
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver
# If intel_pstate, check `turbostat` while engine runs for "PkgWatt" + "PkgTmp"
```

**Mitigation paths:**
- Run engine on a properly-cooled deployment box (server chassis, not laptop).
- If laptop is the only option: undervolt + cap turbo to base clock (`./build.sh`
  with `intel_pstate=disable` boot param + `cpupower frequency-set --max <base>`).
- Disable turbo entirely: `echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo`
  (already in Step 2 above; trades ~10% peak throughput for ~5x lower p99.99 jitter).

**Diagnostic context:** if you see slow-path p99 > 100µs during sustained load
on a properly-tuned (isolcpus + SCHED_FIFO) box, thermal throttling is the most
common cause before architectural bottlenecks. Check thermals BEFORE blaming
code-side latency.

---

## Troubleshooting

### `mlockall` fails with `EAGAIN`
`RLIMIT_MEMLOCK` too low — apply Step 7. Verify with `ulimit -l`.

### `mlockall` fails with `EPERM`
Process lacks `CAP_IPC_LOCK`. Run as root or grant via systemd
`AmbientCapabilities=CAP_IPC_LOCK`.

### `chrt -f 99` fails with "Operation not permitted"
`RLIMIT_RTPRIO` too low — apply Step 3 caveat (`/etc/security/limits.conf`
or systemd `LimitRTPRIO=99`).

### Latency p99 still high after applying everything
Check `/proc/interrupts` — if the isolated cores still show non-zero
counts in any IRQ row, NIC affinity (Step 4) didn't take. Try:
```bash
sudo systemctl status irqbalance   # should be inactive/disabled
```

If `irqbalance` is running and respawning, mask it:
```bash
sudo systemctl mask irqbalance
```

### Engine prints "[poll_interval] WARN" or "[xgb_hyperparams] WARN"
These are from `CoreModelZoo_ValidateAgainstCfg` (v5.10.2.A) — stamp
recorded different cfg than runtime. Either:
1. Retrain the model with current cfg, OR
2. Update cfg to match stamp, OR
3. Set `acknowledge_inference_cfg_drift=1` (Tier 1) or
   `acknowledge_cross_binary_version_drift=1` (Tier 2) in
   `engine.cfg` to suppress.

### Engine prints "[hot_swap] core N REFUSED: ensemble inference active"
Operator triggered a hot model swap on a core that's running multi-
horizon ensemble inference (v5.10.2.B guard). Single-zoo swap would be
a no-op. Restart the engine with the new `core_<i>_model_dir` to swap
the horizon set.

---

## Cross-references

- `DOCS/LATENCY_OPTIMIZATION_AUDIT.md` Parts 6.3 + 13 (source audits)
- `DOCS/STRATEGY_AND_CODING_RULES.md` §8 + §10 + §11 (coding-side rules)
- `DOCS/KNOWN_ISSUES.md` (operational gotchas)
- `engine.cfg.example` (cfg field reference)
- v5.11.0.B engine code: `mlockall` + `RLIMIT_MEMLOCK` preflight at
  `main.cpp:155-194`
- v5.11.0.A engine code: FTZ/DAZ helper at `CoreFrameworks/SystemInit.hpp`

---

## Version history

- v5.11.0a (2026-05-06) — initial document, covers v5.11.0 system
  foundation. Ships in Sprint C alongside the source-side optimizations.
