# 2026-04-25 (evening) — Phase 8b: Operational monitoring + Notify

Branch: `experiment/live-readiness`. Third phase after `phase5d-regression-tests`
and Phase 8a. Continues from tag `phase8a-complete`.

Five commits land here, each tagged `phase8b-c1` … `phase8b-c5`. The last
(c5) is tests; production code ships in c1-c4.

## What ships

The engine can now actively notify on kill switch trips, orphan detection,
and WS disconnects via a configurable backend. Two backends ship:

1. **stderr backend** — fprintf to stderr with `[NOTIFY level]` prefix.
   Default. Visible via `tail -f` or syslog. No setup, no deps.

2. **Command backend** — generic shell-out via popen. Works with any
   notification path that has a CLI: dunst, Discord/Slack/Telegram/ntfy
   webhooks via curl, email via sendmail, custom relays, anything. User
   configures `notify_command` as a shell template with up to two `%s`
   placeholders (subject, body) that get safely shell-escaped.

Default cfg = no behavior change. `notify_enabled=0` keeps `g_notify`
null and all `Notify_Send` call sites become no-ops.

## Scope expansion mid-implementation

The original Phase 8b plan called for native Slack + Telegram backends in
commit 3, with HTTPS POST infrastructure ("reuse TLS pattern from
BinanceCrypto.hpp"). Master plan errata #5 caught that BinanceCrypto is
streaming WSS, not one-shot HTTP POST — implementing native HTTPS POST
would either require ~150 lines of OpenSSL plumbing or a libcurl link
dependency. Errata picked stderr-only initial ship, defer Slack/Telegram
to **Phase 8b.1**.

Mid-c3, switched approach again: replaced the deferral with a generic
**Command backend** (popen-based shell template). This single ~100-line
backend covers Slack/Telegram/Discord/dunst/ntfy.sh/email/anything via
configuration alone. No HTTPS POST infrastructure needed in the engine —
sidesteps the entire master plan errata #5 question. Phase 8b.1 is no
longer needed.

User configures via cfg:

```
notify_enabled=1
notify_backend=1
notify_command=timeout 10 curl -s -X POST -H 'Content-Type: application/json' -d '{"content":"%s\n%s"}' YOUR_DISCORD_WEBHOOK
```

Templates for Discord/Slack/Telegram/dunst/ntfy/email are documented in
`engine.cfg` comments and the Settings panel tooltip.

## Commits

### c1 (`f0e4182`) — Notify.hpp infrastructure

Pluggable-backend notifier with queue + dedicated worker thread:
- 64-event ring buffer, drops on full (warns to stderr).
- Single mutex + condvar; backend called WITHOUT lock so a slow backend
  doesn't block enqueue.
- Per-event-kind cooldown gate. CLOCK_MONOTONIC (master plan errata #4).
- Shutdown drains remaining events before joining worker thread.
- `extern NotifyState* g_notify` declaration; storage initially planned
  for main.cpp (master plan errata #5) — see c2 for the inline-variable
  amendment that resolved a linker issue.

`NotifyKind` enum is **append-only** — adding a kind never renumbers
existing values, so cooldown indexes stay stable across versions.

### c2 (`8463784`) — wire Notify_Send into existing event sites

Six alertable event sites get a `Notify_Send` call alongside their existing
`fprintf`. File logs unchanged.

| Site | Kind | Level |
|---|---|---|
| `PortfolioController.hpp` `[KILL] TRIGGER` | `NK_KILL_TRIGGER` | ALERT |
| `PortfolioController.hpp` `[KILL] daily loss exceeded` | `NK_KILL_DAILY_LOSS` | ALERT |
| `PortfolioController.hpp` `[KILL] drawdown exceeded` | `NK_KILL_DRAWDOWN` | ALERT |
| `main.cpp` orphan force-close fatal | `NK_ORPHAN_HALT` | CRITICAL |
| `main.cpp` orphaned BTC startup recovery | `NK_ORPHAN_DETECTED` | WARN |
| `main.cpp` orphaned real positions | `NK_ORPHAN_DETECTED` | ALERT |
| `BinanceUserData.hpp` WS disconnected | `NK_DISCONNECT_USERDATA` | WARN |
| `BinanceCrypto.hpp` trade WS reconnect | `NK_DISCONNECT_TRADE` | WARN |
| `BinanceDepth.hpp` depth WS disconnect | `NK_DISCONNECT_DEPTH` | WARN |

Disconnect log triplets (UserData has keepalive_failed/frame_read_error/
disconnected) converge on a single `Notify_Send` at the convergence point —
cooldown collapses repeated disconnect storms anyway, so a single Send is
sufficient.

The depth disconnect site (`BinanceDepth.hpp:plen<0`) now has BOTH
`DepthRecorder_LogGap` (Phase 8a) AND `Notify_Send` (this phase) adjacent.
Both conditional on their respective globals being non-null.

**`g_notify` ownership amendment**: original master plan errata #5 specified
`extern` in Notify.hpp + definition in main.cpp. controller_test and
foxml_suite don't link main.cpp, so they couldn't resolve the symbol.
Resolved in-flight by switching to a C++17 inline variable in Notify.hpp
(single definition across TUs, no per-target stub needed).

### c3 (`74b44e1`) — stderr + Command backends + cfg + Init/Shutdown

Two backends ship.

**stderr backend** is trivial — fprintf with level prefix.

**Command backend** is the generic shell-out. Key design decisions:

- `Notify_BuildCommand` does manual `%s` substitution, NEVER snprintf with
  user-supplied template. A user cfg with stray `%d`/`%n` won't read
  uninitialized memory or corrupt the buffer.
- `Notify_ShellEscape` replaces internal `'` with `'\''` (close-escape-
  reopen idiom). Does **NOT** add enclosing quotes — caller's TEMPLATE
  provides them (`'%s'`). Original implementation enclosed and produced
  `''`-pair collisions when substituted into `'%s'`-quoted templates;
  caught by smoke test mid-c3, fixed before commit.
- `popen` blocks the worker thread until the command exits. Document
  recommendation: prepend `timeout 10` in cfg for safety against hung
  curl / network calls.
- `pclose` reaps the child to prevent zombies.
- Stdout from child is drained (small loop fread) so the child doesn't
  block on a full pipe.

Cfg fields added (all opt-in, defaults preserve pre-Phase-8b behavior):
- `notify_enabled` (int, default 0)
- `notify_backend` (int, default 0=stderr; 1=command)
- `notify_command` (string, default "")
- `notify_cooldown_secs` (uint32, default 60)

main.cpp:
- After cfg load, if `notify_enabled=1`: pick backend, `NotifyState_Init`,
  set `g_notify = &g_notify_state`. Falls back to stderr with a warning if
  user picks `backend=1` but leaves `notify_command` empty.
- On engine shutdown: `NotifyState_Shutdown` drains queue + joins worker.

Notify state lives as `static NotifyState g_notify_state` in main() —
function-static, lifetime spans the whole process.

### c4 (this commit) — Settings panel + engine.cfg cookbook + CLAUDE.md

UI exposure for the 4 new cfg fields under a new "Operational Monitoring"
Settings panel section. Tooltips include the cookbook (dunst, Discord,
Slack, Telegram, ntfy.sh templates) so users can copy-paste.

`SettingsPanel.hpp` `path_vals` buffer bumped from 256 → 512 bytes per
field to fit longer notify_command templates (curl + URL + JSON payload
can hit ~200-400 chars). Other CFG_PATH fields (model paths) get the
extra room for free.

engine.cfg gets a documented Operational Alerts section with the same
cookbook in comments. Default `notify_enabled=0` preserves backward compat
for existing engine.cfg files — they parse without errors, behavior
unchanged unless user opts in.

CLAUDE.md gets a new "Operational Alerting" subsection under Safety
Invariants. The 7-point checklist:
1. New events → append to `NotifyKind` enum (never reorder)
2. Call `Notify_Send` alongside existing `fprintf`, don't replace
3. Pick correct level (INFO/WARN/ALERT/CRITICAL)
4. Reuse same kind for same logical event (cooldown is per-kind)
5. NEVER `Notify_Send` from hot path
6. Subject ≤ 128, body ≤ 512, plain ASCII for cross-backend safety
7. Guard `if (g_notify)` — backtest / tests leave it null

### c5 (next) — tests

~14 assertions in `tests/controller_test.cpp` covering the test sidecar's
6 groups: lifecycle, send+dispatch, cooldown, queue full, shutdown drain,
hooked event sites. The Group 6 hooked-site tests verify production code
actually fires the right kinds when triggered (kill switch, disconnects).

## Plan amendments applied

Per cross-plan analysis 2026-04-25 evening + mid-implementation discoveries:

1. CLOCK_MONOTONIC for cooldown (master plan errata #4) — applied in c1.
2. `g_notify` ownership: originally extern + main.cpp def per errata #5;
   amended in c2 to C++17 inline variable to fix linker issue across test
   binaries.
3. stderr-only first ship (master plan errata): originally upheld in c3,
   but EXPANDED to also include the generic Command backend after
   recognizing it sidesteps HTTPS-POST infrastructure entirely. Phase 8b.1
   deferral is now unnecessary.
4. `BinanceUserData.hpp:397` removed from log site list (it's a counter
   `fetch_add`, not a log line) — verified during c2 and applied.
5. Snapshot sync rule: not relevant to Phase 8b (no TUISnapshot fields
   added).

## Known limitations

- **JSON escaping not done** — `"` and `\` in alert text break Discord/
  Slack JSON payloads. Engine-generated alerts are plain ASCII so this
  is rarely an issue. Users with custom message content + JSON backends
  should wrap their curl in a helper script that does proper JSON
  escaping (`jq -Rs` or `python -c 'import json,sys;...'`).
- **popen blocks the worker thread** until the command exits. Recommend
  prepending `timeout 10 ` in the cfg template. A 10s pause on a hung
  alert will queue up to 64 backed-up events before drops start.
- **Per-service template presets** are documented in cfg comments + tooltip
  but not pre-loaded as named cfg fields — the user copy-pastes the right
  one for their service. Future cookbook entry could ship preset commands
  as separate cfg fields if popular request.
- **No retry on backend failure** — fail forward, drop the event, continue.
  Add exponential-backoff retry only if reliability needs justify it.

## Anti-drift verified

Every commit in c1-c4:
- `ML_Headers/ModelInference.hpp::ModelFeatures_Pack` unchanged
- `ML_Headers/RollingStats.hpp::RollingStats_Push` unchanged
- `CoreFrameworks/ExecutionCore.hpp::ExecutionCore_Tick` unchanged
- `FEAT_*` constants unchanged
- `controller_test` 296/296 (post-Phase-5d baseline)
- `depth_recorder_test` 17/17 (Phase 8a baseline)
- All 4 main targets build clean
- `notify_enabled=0` (default) → no behavior change for existing cfg

## Tags

`phase8b-c1` … `phase8b-c5` mark each commit. `phase8b-complete` will tag
at the end of c5 (tests).
