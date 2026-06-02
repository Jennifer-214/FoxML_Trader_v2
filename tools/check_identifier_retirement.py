#!/usr/bin/env python3
"""
check_identifier_retirement.py — the tombstone CI guard (Knight-Capital defense).

WHY THIS EXISTS
---------------
Persistence/wire-visible identifiers — snapshot/format VERSION numbers, and the
integer CODES of persisted/logged/wire-emitted enum registries — must NEVER be
renumbered, value-reused, or silently removed. An OLD persisted file, an OLD
wire/HMAC message, or an un-updated node can carry the OLD meaning of a reused
identifier and silently activate the WRONG code path. That is the Knight Capital
failure mode ($440M / 45 min, 2012): a dormant identifier ("Power Peg") reused
for new behavior while old code keyed to it was still compiled in, on a node
that didn't get the deploy. Three ingredients — dead code left in, an identifier
repurposed, deploy/state skew — and a trading firm died.

This guard freezes the current identifier->value map in a golden ledger
(tools/identifier_ledger.txt) and FAILS on any RENUMBER, VALUE-REUSE, or silent
REMOVAL. ADDING a new identifier is fine (append-only) — record it with --update.
RETIRING one is fine too — but you TOMBSTONE the slot (RESERVED / LEGACY_ /
DEPRECATED comment, never reassign the number), and the ledger keeps the row.

The codebase ALREADY practices this informally (BanditAlgorithm "OPTION C
wire-byte preservation"; "RESERVED (was PER_CORE_OK ...)"; LEGACY_CONFIDENCE_VERSION;
StrategyInterface.hpp's "IDs are append-only — never reorder or remove. Persisted
snapshots and trade logs reference these by integer."). This tool MECHANIZES that
existing-but-unenforced convention. Golden-master, not a reimplemented oracle
(feedback_golden_master_over_reimplemented_oracle): the ledger is the frozen REAL
state. See DESIGN_SPECS/meta-disciplines/dead-code-and-identifier-retirement-discipline.md.

COVERAGE (v1): snapshot/format VERSION #defines + STAMP_FORMAT_VERSION_CURRENT +
the persisted ENUM registries (BanditAlgorithm / StrategyId / RegimeId). Bitmap
bit-assignments + cfg-field name keys enroll next — add a SOURCES row (paced
enrollment; the discipline is complete, coverage grows — sister to the H15
meta-registry enrollment pattern).

USAGE
  check_identifier_retirement.py            verify code matches ledger (CI; exit 1 on violation)
  check_identifier_retirement.py --update   regenerate the ledger from current code (after an intentional ADD/tombstone)
  check_identifier_retirement.py --print    print the parsed current map; do not compare

Machine-portable: repo root from $FOXML_REPO_ROOT or derived from this file's
location (feedback_machine_portable_resolver_for_committed_tool_paths).
"""
import os, re, sys

REPO_ROOT = os.environ.get("FOXML_REPO_ROOT") or os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
LEDGER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "identifier_ledger.txt")

# Enrolled identifier sources. Add a row to enroll a new registry.
#   (category, file_relative_to_repo_root, kind, name_or_macro, opts)
# kind: "define"    -> #define NAME <int>
#       "constexpr" -> static constexpr <type> NAME = <int>
#       "foreach"   -> #define MACRO(X) X(name, ...) X(name, ...) ...
#                       opts: prefix, value="explicit"|"positional", value_col (explicit only)
SOURCES = [
    ("version", "CoreFrameworks/ShardedSnapshotPersist.hpp", "define",    "SHARDED_SNAPSHOT_VERSION",    {}),
    ("version", "CoreFrameworks/PortfolioController.hpp",     "define",    "CONTROLLER_SNAPSHOT_VERSION", {}),
    ("version", "CoreFrameworks/Portfolio.hpp",              "define",    "PORTFOLIO_SNAPSHOT_VERSION",  {}),
    ("version", "ML_Headers/ModelInference.hpp",             "define",    "MODEL_FORMAT_VERSION",        {}),
    ("version", "ML_Headers/BanditLearning.hpp",             "define",    "BANDIT_STATE_FORMAT_VERSION", {}),
    ("version", "ML_Headers/ModelInference.hpp",             "constexpr", "STAMP_FORMAT_VERSION_CURRENT",{}),
    ("enum:BanditAlgorithm", "ML_Headers/BanditAlgorithmRegistry.hpp", "foreach", "FOREACH_BANDIT_ALGORITHM",
        {"prefix": "BANDIT_ALGO_", "value": "explicit", "value_col": 1}),
    ("enum:StrategyId", "Strategies/StrategyInterface.hpp", "foreach", "FOREACH_STRATEGY",
        {"prefix": "STRATEGY_", "value": "positional"}),
    ("enum:RegimeId",   "Strategies/StrategyInterface.hpp", "foreach", "FOREACH_REGIME",
        {"prefix": "REGIME_", "value": "positional"}),
]

# Categories whose values are monotonic-non-decreasing (a DROP is also a violation).
MONOTONIC = {"version"}


def _read(rel):
    with open(os.path.join(REPO_ROOT, rel)) as f:
        return f.read()


def _parse_define(text, name):
    m = re.search(r'^\s*#\s*define\s+' + re.escape(name) + r'\s+(\d+)', text, re.M)
    return {name: int(m.group(1))} if m else {}


def _parse_constexpr(text, name):
    m = re.search(r'constexpr\s+\w+\s+' + re.escape(name) + r'\s*=\s*(\d+)', text)
    return {name: int(m.group(1))} if m else {}


def _macro_body(text, macro):
    """Return the backslash-continued body of `#define MACRO(X) ...`, or None."""
    m = re.search(r'#\s*define\s+' + re.escape(macro) + r'\s*\(\s*X\s*\)', text)
    if not m:
        return None
    lines = []
    for line in text[m.end():].splitlines():
        lines.append(line)
        if not line.rstrip().endswith('\\'):
            break
    return "\n".join(lines)


def _rows(body):
    """Split a macro body into the inner-text of each top-level `X( ... )` invocation."""
    out, i = [], 0
    while True:
        j = body.find("X(", i)
        if j < 0:
            break
        depth, k = 0, j + 1
        while k < len(body):
            if body[k] == '(':
                depth += 1
            elif body[k] == ')':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        out.append(body[j + 2:k])
        i = k + 1
    return out


def _args(inner):
    """Top-level comma split (respecting nested parens)."""
    args, depth, cur = [], 0, []
    for c in inner:
        if c == '(':
            depth += 1; cur.append(c)
        elif c == ')':
            depth -= 1; cur.append(c)
        elif c == ',' and depth == 0:
            args.append("".join(cur).strip()); cur = []
        else:
            cur.append(c)
    args.append("".join(cur).strip())
    return args


def _parse_foreach(text, macro, opts):
    body = _macro_body(text, macro)
    if body is None:
        return {}
    out, idx = {}, 0
    for inner in _rows(body):
        args = _args(inner)
        name0 = args[0] if args else ""
        if not re.match(r'^[A-Z0-9_]+$', name0):
            continue  # skip nested macro invocations / non-row tokens
        full = opts["prefix"] + name0
        if opts.get("value") == "explicit":
            try:
                val = int(args[opts["value_col"]])
            except (ValueError, IndexError):
                continue
        else:
            val = idx
        out[full] = val
        idx += 1
    return out


def parse_current():
    """Return {category: {identifier_name: int_value}} parsed from the live source."""
    result = {}
    for category, rel, kind, name, opts in SOURCES:
        text = _read(rel)
        if kind == "define":
            got = _parse_define(text, name)
        elif kind == "constexpr":
            got = _parse_constexpr(text, name)
        elif kind == "foreach":
            got = _parse_foreach(text, name, opts)
        else:
            sys.exit(f"unknown source kind: {kind}")
        if not got:
            sys.exit(f"PARSE ERROR: found nothing for {category} ({kind} {name}) in {rel} "
                     f"— the source moved/renamed; update SOURCES in {os.path.basename(__file__)}.")
        result.setdefault(category, {}).update(got)
    return result


def load_ledger():
    frozen = {}
    if not os.path.exists(LEDGER):
        return None
    with open(LEDGER) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cat, name, val = line.split("|")
            frozen.setdefault(cat, {})[name] = int(val)
    return frozen


def write_ledger(current):
    lines = [
        "# identifier_ledger.txt — the tombstone golden (check_identifier_retirement.py).",
        "# Persistence/wire-visible identifier -> value. NEVER renumber/reuse/drop a row;",
        "# tombstone retired slots (RESERVED/LEGACY_/DEPRECATED) and keep the row. ADD = append.",
        "# Regenerate after an intentional ADD/tombstone: tools/check_identifier_retirement.py --update",
        "# format: category|identifier|value",
        "",
    ]
    for cat in sorted(current):
        for name in sorted(current[cat], key=lambda n: (current[cat][n], n)):
            lines.append(f"{cat}|{name}|{current[cat][name]}")
        lines.append("")
    with open(LEDGER, "w") as f:
        f.write("\n".join(lines).rstrip() + "\n")


def compare(frozen, current):
    """Violations = renumber / value-reuse / silent removal / version-decrease.
       Additions (new identifier) + version bumps (monotonic increase) are OK — info only."""
    violations, additions, bumps = [], [], []
    for cat, names in frozen.items():
        cur = current.get(cat, {})
        monotonic = cat in MONOTONIC          # versions bump up legitimately; enums never change value
        cur_by_val = {}
        if not monotonic:
            for n, v in cur.items():
                cur_by_val.setdefault(v, []).append(n)
        for name, val in names.items():
            if name not in cur:
                violations.append(
                    f"REMOVED  {cat} :: {name} (was {val}) — a persisted/wire identifier vanished. "
                    f"Old state/messages still reference {val}; TOMBSTONE the slot (RESERVED/LEGACY_), do not drop the row.")
            elif monotonic:
                if cur[name] < val:
                    violations.append(
                        f"DECREASED {cat} :: {name} {val} -> {cur[name]} — a format version went BACKWARDS; "
                        f"old-layout state/messages would silently reload into new code (Knight-Capital stale-state).")
                elif cur[name] > val:
                    bumps.append(f"{cat} :: {name} {val} -> {cur[name]} (version bump)")
                # equal -> unchanged, fine
            elif cur[name] != val:
                violations.append(
                    f"RENUMBERED {cat} :: {name} {val} -> {cur[name]} — a persisted enum code is immutable "
                    f"(Knight-Capital reuse). Allocate a NEW identifier for the new meaning; never re-stamp an old one.")
            else:
                # name held its value; check nobody ELSE grabbed that value (reuse via drop+re-add)
                holders = [n for n in cur_by_val.get(val, []) if n != name]
                if holders:
                    violations.append(
                        f"VALUE-REUSE {cat} :: value {val} now also held by {holders} (frozen owner: {name}).")
    for cat, names in current.items():
        for name, val in names.items():
            if name not in frozen.get(cat, {}):
                additions.append(f"{cat} :: {name} = {val}")
    return violations, additions, bumps


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else ""
    current = parse_current()

    if arg == "--print":
        for cat in sorted(current):
            for name in sorted(current[cat], key=lambda n: current[cat][n]):
                print(f"{cat}|{name}|{current[cat][name]}")
        return 0

    if arg == "--update":
        write_ledger(current)
        n = sum(len(v) for v in current.values())
        print(f"[identifier-retirement] ledger regenerated: {n} identifiers across {len(current)} categories -> {LEDGER}")
        return 0

    frozen = load_ledger()
    if frozen is None:
        print(f"[identifier-retirement] NO LEDGER at {LEDGER} — run --update once to establish the golden.", file=sys.stderr)
        return 1

    violations, additions, bumps = compare(frozen, current)
    for a in additions:
        print(f"[identifier-retirement] ADD (ok; run --update to record): {a}")
    for b in bumps:
        print(f"[identifier-retirement] BUMP (ok; run --update to record): {b}")
    if violations:
        print("\n=== IDENTIFIER-RETIREMENT VIOLATION (Knight-Capital tombstone guard) ===", file=sys.stderr)
        for v in violations:
            print(f"  ✗ {v}", file=sys.stderr)
        print("\nPersisted/wire-visible identifiers are append-only + immutable. To retire one, tombstone the\n"
              "slot (RESERVED/LEGACY_/DEPRECATED, keep the number) — never renumber or reuse it. If this change\n"
              "is intentional + safe, re-run with --update to re-freeze the ledger.\n", file=sys.stderr)
        return 1

    total = sum(len(v) for v in current.values())
    print(f"[identifier-retirement] GREEN — {total} persisted/wire identifiers match the ledger; no renumber/reuse/drop.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
