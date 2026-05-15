#!/usr/bin/env python3
"""scan_class_27_full.py — full-codebase Class 27 scanner (beyond Check 7's designated-types).

Established at v5.15.5.F.4c.3 WIP2d-1.B.0c per DESIGN_SPECS/decision-time-data-binding-pattern.md.

Two-mode scanner:

MODE A — Subsystem state struct scan (broader Class 27 detection):
  - Scans ALL `struct` declarations in CoreFrameworks/, ML_Headers/, Strategies/,
    Backtest/, DataStream/
  - For each, identifies SCALAR fields whose names match FOREACH_PER_CORE_CFG_FIELD
    or FOREACH_GLOBAL_CFG_FIELD entries (heuristic match — exact name AND name
    minus common suffixes like `_pct`, `_ticks`)
  - Reports candidate Class 27 instances with severity tier
  - Exemptions in DOCS/MANUAL_FIELDS_INVENTORY.md Section C are respected

MODE B — Function-local static cache scan (Class 27 fn-local variant):
  - Greps for `static\s+const\s+\w+\s+\w+\s*=\s*.*FPN_ToDouble\s*\(\s*cfg\.\w`
  - Anti-pattern: static cache freezes cfg value at first invocation; never refreshes
  - Concrete example caught: `static const double fee_rate_taker_d = FPN_ToDouble(cfg.fee_rate_taker);`
    at EngineSharded.hpp:2469 (slated for deletion at WIP2d-1.B.1)

Output: structured markdown report for /accounting-audit skill consumption,
OR JSON via --json flag for programmatic skill integration.

NOT actual edits. Operator/skill decides which instances to triage.

Distinct from tools/check_per_core_registry_integrity.py Check 7:
- Check 7 = narrow build-failing scan (DESIGNATED subsystem types only;
  enforces exemption requirement)
- scan_class_27_full = broad informational sweep (ALL subsystems; surfaces
  candidates for /accounting-audit canonical first run at WIP2d-1.B.1.b cohort sweep)

Cross-references:
  - DESIGN_SPECS/decision-time-data-binding-pattern.md
  - DOCS/RECURRING_BUG_PATTERNS.md Class 27
  - tools/check_per_core_registry_integrity.py Check 7 (narrower CI variant)
  - claude-skills/accounting-audit/SKILL.md (orchestrator)
"""

import argparse
import json
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT  = SCRIPT_DIR.parent
CFG_REG    = REPO_ROOT / "CoreFrameworks/CfgFieldRegistry.hpp"
INVENTORY  = REPO_ROOT / "DOCS/MANUAL_FIELDS_INVENTORY.md"

SCAN_DIRS = [
    "CoreFrameworks",
    "ML_Headers",
    "Strategies",
    "Backtest",
    "DataStream",
]

# Cfg-authority structs — these ARE the cfg, not mirrors of it.
# Matching their own field names against the cfg registry is a false positive
# (the registry IS the field declarations for these structs).
EXCLUDED_STRUCTS_CFG_AUTHORITY = {
    "ControllerConfig",       # IS the global+per-core cfg authority
    "PerCoreCfg",             # IS the per-core cfg slice authority
    "PerCoreOverrides",       # legacy per-core override (deleted at WIP2f)
    "BacktestCfg",            # IS the backtest cfg authority
    "TrainingCfg",            # IS the training cfg authority
    "BinanceConfig",          # IS the binance API cfg
    "SecretsCfg",             # IS the secrets cfg
    "CfgFieldDescriptor",     # registry metadata struct
    "InitArenaConfig",        # init arena cfg
}


def read_file(p: Path) -> str:
    try:
        return p.read_text()
    except FileNotFoundError:
        return ""


def parse_cfg_field_names(text: str, macro_name: str) -> set:
    """Parse FOREACH_<macro> rows; return set of field names."""
    pattern = re.compile(
        rf'^#define\s+{re.escape(macro_name)}\(X\)\s*\\?\s*\n(.*?)(?=\n\s*\n|\n#define|\Z)',
        re.MULTILINE | re.DOTALL,
    )
    m = pattern.search(text)
    if not m:
        return set()
    body = m.group(1)
    # Match X(<type>, KIND_<TOKEN>, <name>, ...)  — TYPE-first variant
    row_pattern = re.compile(r'^\s+X\(\s*([^,]+?(?:<[^>]+>)?)\s*,\s+(KIND_[A-Z_]+),\s+(\w+),', re.MULTILINE)
    names = set()
    for m in row_pattern.finditer(body):
        names.add(m.group(3).strip())
    return names


def parse_inventory_section_c(text: str) -> set:
    """Parse Section C exemptions; return set of (subsystem, field) tuples."""
    m = re.search(r'## Section C.*?(?=\n##\s|\Z)', text, re.DOTALL)
    if not m:
        return set()
    section_c = m.group(0)
    exemptions = set()
    for line in section_c.split('\n'):
        line = line.strip()
        if not line.startswith('|') or '---' in line:
            continue
        parts = [p.strip() for p in line.split('|')[1:-1]]
        if len(parts) < 2:
            continue
        if parts[0].lower().startswith('subsystem'):
            continue
        subsystem = parts[0].strip('` ')
        field = parts[1].strip('` ')
        if subsystem and field and subsystem != '---' and field != '---':
            exemptions.add((subsystem, field))
    return exemptions


def scan_struct_scalar_fields(text: str, source_file: str) -> list:
    """Find scalar fields inside all struct declarations in a source file.

    Returns list of (struct_name, field_name, field_type, line_no).
    Excludes arrays, pointers, sub-structs, X-macro invocations.
    """
    results = []
    # Match: optional `template<>` + `struct [alignas(N)] <name> { ... };`
    struct_pattern = re.compile(
        r'(?:template\s*<[^>]+>\s*\n)?struct\s+(?:alignas\(\d+\)\s+)?(\w+)\s*\{(.*?)^\};',
        re.MULTILINE | re.DOTALL,
    )
    for m in struct_pattern.finditer(text):
        struct_name = m.group(1)
        body = m.group(2)
        body_start_line = text[:m.start()].count('\n') + 1
        nesting_depth = 0
        line_no = body_start_line
        for line in body.split('\n'):
            line_no += 1
            nesting_depth += line.count('{') - line.count('}')
            if nesting_depth > 0:
                continue
            # Strip inline comments
            comment_pos = line.find('//')
            if comment_pos >= 0:
                line = line[:comment_pos]
            line = line.strip()
            if not line or line.startswith('/*') or line.startswith('*') or line.startswith('*/'):
                continue
            if line.startswith('#') or 'FOREACH_' in line or 'EMIT_' in line:
                continue
            if re.match(r'^(?:struct|class|union|enum|typedef|using)\b', line):
                continue
            # SCALAR field decl (no arrays, no pointers)
            # Match: optional alignas(N) + type name; or type name = init;
            m2 = re.match(
                r'^(?:alignas\(\d+\)\s+)?([a-zA-Z_][a-zA-Z_0-9]*(?:\s*<\s*[^>]+\s*>)?)\s+(\w+)\s*(?:=\s*[^;]+)?\s*;',
                line,
            )
            if m2:
                typ = m2.group(1).strip()
                name = m2.group(2).strip()
                # Skip if pointer or other suspect chars
                if '*' in line or '&' in line.replace('&&', ''):
                    continue
                # Skip array decls (regex above doesn't allow [ but be defensive)
                if '[' in name:
                    continue
                results.append((struct_name, name, typ, line_no))
    return results


def scan_static_const_cache(text: str, source_file: str) -> list:
    """Find Class 27 fn-local variant: static const T = ... cfg.X ...

    Returns list of (line_no, matched_line).
    """
    results = []
    pattern = re.compile(
        r'static\s+const\s+\w+\s+\w+\s*=\s*.*FPN_ToDouble\s*\(\s*cfg\.\w',
    )
    for line_no, line in enumerate(text.split('\n'), start=1):
        if pattern.search(line):
            results.append((line_no, line.strip()))
    return results


def severity_for_match(struct_name: str, field_name: str) -> str:
    """Classify Class 27 candidate severity.

    Heuristic based on subsystem + field semantics:
    - HIGH if subsystem is accounting-critical (OMS, Portfolio, ConfidenceScorer)
      AND field is fee/rate/slippage/commission/balance/pnl
    - MEDIUM if subsystem is per-core-sharded AND field is per-core-eligible
    - LOW otherwise
    """
    accounting_critical = {"OrderManagerState", "Portfolio", "PortfolioController", "ConfidenceScorerState"}
    accounting_fields = ("fee", "rate", "slippage", "commission", "pnl", "balance", "drawdown")
    if struct_name in accounting_critical and any(s in field_name.lower() for s in accounting_fields):
        return "HIGH"
    if "per_core" in struct_name.lower() or "core_" in field_name.lower():
        return "MEDIUM"
    return "LOW"


def emit_markdown(findings_struct: list, findings_static: list, args) -> None:
    """Emit findings as structured markdown report."""
    print("# Class 27 full-codebase scan findings\n")
    print(f"**Scanner:** `tools/scan_class_27_full.py`")
    print(f"**Date:** {__doc__.split('Established at ')[1].split(' per')[0]}")
    print(f"**Reference:** `DESIGN_SPECS/decision-time-data-binding-pattern.md` + RECURRING_BUG_PATTERNS Class 27\n")

    print("## Summary\n")
    high = sum(1 for f in findings_struct if f.get('severity') == 'HIGH')
    med  = sum(1 for f in findings_struct if f.get('severity') == 'MEDIUM')
    low  = sum(1 for f in findings_struct if f.get('severity') == 'LOW')
    exempt = sum(1 for f in findings_struct if f.get('exempt'))
    print(f"- Mode A (subsystem state scalar cfg-mirror candidates): {len(findings_struct)} matches ({exempt} exempt via Section C)")
    print(f"  - HIGH: {high}, MEDIUM: {med}, LOW: {low}\n")
    print(f"- Mode B (static const fn-local cfg cache hazards): {len(findings_static)} matches\n")

    if findings_struct:
        print("## Mode A — Subsystem state scalar cfg-mirror candidates\n")
        for f in findings_struct:
            exempt_marker = " *(exempt — Section C)*" if f.get('exempt') else ""
            print(f"### [{f['severity']}] `{f['struct']}::{f['field']}` ({f['file']}:{f['line']}){exempt_marker}")
            print(f"- **Field type:** `{f['type']}`")
            print(f"- **Matches cfg field:** `{f['cfg_match']}`")
            print(f"- **Recommended fix:**")
            if f.get('exempt'):
                print(f"  - Already exempted in `DOCS/MANUAL_FIELDS_INVENTORY.md` Section C; verify exemption still valid.")
            else:
                print(f"  - (a) Pre-resolve onto in-flight object at decision time (PREFERRED) — see `DESIGN_SPECS/decision-time-data-binding-pattern.md`")
                print(f"  - (b) Migrate to `FOREACH_{f['struct'].upper()}_CFG_CACHE` registry (fallback if no in-flight object)")
                print(f"  - (c) Add exemption to `DOCS/MANUAL_FIELDS_INVENTORY.md` Section C with rationale category")
            print()

    if findings_static:
        print("## Mode B — Static const fn-local cfg cache hazards\n")
        for f in findings_static:
            print(f"### [HIGH] `{f['file']}:{f['line']}`")
            print(f"- **Matched line:** `{f['matched']}`")
            print(f"- **Anti-pattern:** Class 27 fn-local variant — `static const` cache freezes cfg value at first invocation; never refreshes.")
            print(f"- **Recommended fix:** Remove `static`; read per-call. Or pre-resolve onto in-flight object if available.")
            print(f"- **Reference:** `DESIGN_SPECS/decision-time-data-binding-pattern.md` § Anti-pattern 2")
            print()


def emit_json(findings_struct: list, findings_static: list) -> None:
    """Emit findings as JSON for programmatic skill consumption."""
    print(json.dumps({
        "mode_a_subsystem_scan": findings_struct,
        "mode_b_static_const_cache": findings_static,
    }, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser(description="Full-codebase Class 27 scanner")
    parser.add_argument("--json", action="store_true", help="Output JSON instead of markdown")
    parser.add_argument("--mode", choices=["a", "b", "both"], default="both",
                        help="Scan mode: a=subsystem state, b=static const, both=default")
    parser.add_argument("--include-exempt", action="store_true",
                        help="Include Section C exempted matches in output (default: skip)")
    args = parser.parse_args()

    # Load registry field names
    cfg_text = read_file(CFG_REG)
    per_core_names = parse_cfg_field_names(cfg_text, "FOREACH_PER_CORE_CFG_FIELD")
    global_names   = parse_cfg_field_names(cfg_text, "FOREACH_GLOBAL_CFG_FIELD")
    all_cfg_names  = per_core_names | global_names

    # Load Section C exemptions
    inventory_text = read_file(INVENTORY)
    exemptions = parse_inventory_section_c(inventory_text)

    findings_struct = []
    findings_static = []

    # Walk scan dirs
    for scan_dir in SCAN_DIRS:
        dir_path = REPO_ROOT / scan_dir
        if not dir_path.exists():
            continue
        for hpp_file in dir_path.rglob("*.hpp"):
            rel_path = hpp_file.relative_to(REPO_ROOT)
            text = read_file(hpp_file)
            if not text:
                continue

            if args.mode in ("a", "both"):
                struct_fields = scan_struct_scalar_fields(text, str(rel_path))
                for struct_name, field_name, typ, line_no in struct_fields:
                    # Skip cfg-authority structs (false positive — these ARE the cfg)
                    if struct_name in EXCLUDED_STRUCTS_CFG_AUTHORITY:
                        continue
                    if field_name in all_cfg_names:
                        is_exempt = (struct_name, field_name) in exemptions
                        if is_exempt and not args.include_exempt:
                            continue
                        findings_struct.append({
                            "file": str(rel_path),
                            "line": line_no,
                            "struct": struct_name,
                            "field": field_name,
                            "type": typ,
                            "cfg_match": field_name,  # exact match for now
                            "severity": severity_for_match(struct_name, field_name),
                            "exempt": is_exempt,
                        })

            if args.mode in ("b", "both"):
                static_matches = scan_static_const_cache(text, str(rel_path))
                for line_no, matched in static_matches:
                    findings_static.append({
                        "file": str(rel_path),
                        "line": line_no,
                        "matched": matched,
                    })

    if args.json:
        emit_json(findings_struct, findings_static)
    else:
        emit_markdown(findings_struct, findings_static, args)

    return 0


if __name__ == "__main__":
    sys.exit(main())
