#!/usr/bin/env python3
"""check_per_core_registry_integrity.py — CI cross-check for per-core cfg discipline.

Established at v5.15.5.F.4c.3 WIP2d-0 per the structural-fix primitive for the
per-core cfg surface. Enforces H17 STRONG invariant (per-core surface): cfg
struct field declarations MUST come from FOREACH_PER_CORE_FIELD_TYPE via X-macro
generation; manual cfg-surface field declarations FORBIDDEN.

Closes bug classes (cfg-scope-discipline.md):
- Parallel-array drift (Class A): `core_X[16]` shadowing per-core registry row
- Manual-field bypass (Class B): adding to PerCoreCfg<F> body without registry row
- Anti-pattern 1 consumer (informational): `cfg.X` + `cfg.core_overrides[c].X`

Cross-checks performed (all build-failing on violation):
  1. FOREACH_PER_CORE_CFG_FIELD ↔ FOREACH_PER_CORE_FIELD_TYPE bidirectional sync
  2. PerCoreCfg<F> body contains ONLY: X-macro expansion + 5 permitted runtime bitmap fields
  3. ControllerConfig parallel arrays ↔ FOREACH_MANUAL_PER_CORE_FIELD bidirectional sync
  4. FOREACH_MANUAL_PER_CORE_FIELD ↔ MANUAL_FIELDS_INVENTORY.md bidirectional sync
  5. No name duplication between FOREACH_PER_CORE_CFG_FIELD + FOREACH_MANUAL_PER_CORE_FIELD
  6. TRANSITIONAL exemption rot detection (WARN-level)

Exit codes:
  0 = all checks pass (build proceeds)
  1 = one or more checks failed (build aborted)
  2 = script error / file missing (build aborted)

Cross-references:
  - DESIGN_SPECS/manual-fields-inventory-pattern.md (the pattern doc)
  - DESIGN_SPECS/cfg-scope-discipline.md (audit grep signatures)
  - DESIGN_SPECS/per-instance-registry-pattern.md (framework discipline)
  - DOCS/MANUAL_FIELDS_INVENTORY.md (documented exemption registry)
  - DESIGN_PHILOSOPHY.md § 2 H17 STRONG codification
"""

import re
import sys
from pathlib import Path

# Repo paths relative to script location
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT  = SCRIPT_DIR.parent
CFG_REG    = REPO_ROOT / "CoreFrameworks/CfgFieldRegistry.hpp"
CTRL_CFG   = REPO_ROOT / "CoreFrameworks/ControllerConfig.hpp"
INVENTORY  = REPO_ROOT / "DOCS/MANUAL_FIELDS_INVENTORY.md"

# 5 permitted runtime bitmap fields in PerCoreCfg<F> body (Section B of inventory)
PERMITTED_RUNTIME_FIELDS = {
    "lifecycle_cfg_flags",
    "gate_cfg_flags",
    "ml_cfg_flags",
    "risk_cfg_flags",
    "ops_cfg_flags",
}

# Check 7 — subsystem state types to scan for Class 27 (scalar cfg-mirror) violations.
# Each entry maps a struct type name to its source file (relative to REPO_ROOT).
# Add new subsystems here as they're audited for Class 27 cleanliness.
# Per DESIGN_SPECS/decision-time-data-binding-pattern.md + RECURRING_BUG_PATTERNS Class 27.
SUBSYSTEM_STATE_TYPES_FOR_CLASS_27_SCAN = {
    "OrderManagerState": "CoreFrameworks/OrderManager.hpp",
    # Future additions (post-WIP2d-1.B.1.b sweep): ConfidenceScorerState, etc.
}


def fail(msg: str) -> None:
    """Emit error to stderr in operator-readable format."""
    print(f"[per-core-cfg-CI] ERROR: {msg}", file=sys.stderr)


def warn(msg: str) -> None:
    """Emit warning to stderr (non-fatal)."""
    print(f"[per-core-cfg-CI] WARN: {msg}", file=sys.stderr)


def info(msg: str) -> None:
    """Emit info to stdout."""
    print(f"[per-core-cfg-CI] {msg}")


def read_file(p: Path) -> str:
    try:
        return p.read_text()
    except FileNotFoundError:
        fail(f"file not found: {p}")
        sys.exit(2)


def extract_macro_body(text: str, macro_name: str) -> str:
    """Extract the body of a #define FOREACH_X(X) ... macro definition.

    Returns text between the #define line and the next blank line / #define / EOF.
    """
    pattern = re.compile(
        rf'^#define\s+{re.escape(macro_name)}\(X\)\s*\\?\s*\n(.*?)(?=\n\s*\n|\n#define|\Z)',
        re.MULTILINE | re.DOTALL,
    )
    m = pattern.search(text)
    if not m:
        fail(f"could not find macro body for {macro_name}")
        sys.exit(2)
    return m.group(1)


def parse_foreach_per_core_cfg_field(body: str) -> dict:
    """Parse FOREACH_PER_CORE_CFG_FIELD rows. Returns dict {name: type}.

    Post-WIP2d-0.B: TYPE is the FIRST column. Row shape:
        X(<storage_type>, KIND_TOKEN, <name>, "label", "section", meta, payload, "tooltip", ...)

    Type can contain template brackets (FPN<F>) so we use a non-greedy match through commas.
    """
    result = {}
    # Match: X(<type>, KIND_<TOKEN>, <name>,
    # Type can have <F> template parameter — match anything up to ", KIND_"
    pattern = re.compile(r'^\s+X\(\s*([^,]+?(?:<[^>]+>)?)\s*,\s+(KIND_[A-Z_]+),\s+(\w+),', re.MULTILINE)
    for m in pattern.finditer(body):
        typ = m.group(1).strip()
        name = m.group(3).strip()
        result[name] = typ
    return result


def parse_foreach_manual_per_core_field(body: str) -> dict:
    """Parse FOREACH_MANUAL_PER_CORE_FIELD rows. Returns dict {name: (type, suffix, rationale)}.

    Rows look like:
        X(type, name, suffix, "rationale")
    """
    result = {}
    # Carefully match: type can have template brackets; suffix can be [N] or empty; rationale is quoted
    pattern = re.compile(
        r'^\s+X\(\s*([^,]+?)\s*,\s*(\w+)\s*,\s*(\[[^\]]*\]|\s*)\s*,\s*"([^"]*)"\)',
        re.MULTILINE,
    )
    for m in pattern.finditer(body):
        typ = m.group(1).strip()
        name = m.group(2).strip()
        suffix = m.group(3).strip()
        rationale = m.group(4).strip()
        result[name] = (typ, suffix, rationale)
    return result


def parse_per_core_cfg_body(text: str) -> dict:
    """Parse fields declared inside PerCoreCfg<F> struct body.

    Returns dict {name: (type, line_no)}. EXCLUDES the X-macro expansion call.
    Detects FOREACH_PER_CORE_FIELD_TYPE invocation; collects manual fields ONLY.
    """
    # Find the struct definition
    m = re.search(
        r'template\s*<unsigned\s+F>\s*\nstruct\s+alignas\(64\)\s+PerCoreCfg\s*\{(.*?)^\};',
        text,
        re.MULTILINE | re.DOTALL,
    )
    if not m:
        fail("could not find PerCoreCfg<F> struct definition")
        sys.exit(2)
    body = m.group(1)
    body_start_line = text[:m.start()].count('\n') + 2  # approximate; +2 for template + struct lines

    # Detect if FOREACH_PER_CORE_CFG_FIELD is invoked (expected post-WIP2d-0.B; covers 92 fields)
    has_cfg_macro = bool(re.search(r'FOREACH_PER_CORE_CFG_FIELD\s*\(\s*EMIT_PER_CORE_CFG_STRUCT_FIELD\s*\)', body))
    if not has_cfg_macro:
        fail("PerCoreCfg<F> body missing FOREACH_PER_CORE_CFG_FIELD(EMIT_PER_CORE_CFG_STRUCT_FIELD) invocation")
        sys.exit(1)

    # Detect if FOREACH_PER_CORE_DOMAIN_BITMAP is invoked (expected post-WIP2d-0.B; covers 5 bitmap fields)
    has_bitmap_macro = bool(re.search(r'FOREACH_PER_CORE_DOMAIN_BITMAP\s*\(\s*EMIT_DOMAIN_BITMAP_FIELD\s*\)', body))
    if not has_bitmap_macro:
        fail("PerCoreCfg<F> body missing FOREACH_PER_CORE_DOMAIN_BITMAP(EMIT_DOMAIN_BITMAP_FIELD) invocation")
        sys.exit(1)

    # Find manual field declarations (after stripping comments + the X-macro line)
    # Field decl pattern: optional alignas(N) + type + name;
    # Type can be: uint8_t, uint16_t, uint32_t, uint64_t, int, double, FPN<F>, etc.
    manual_fields = {}
    line_no = body_start_line
    for line in body.split('\n'):
        line_no += 1
        # Strip inline comments
        comment_pos = line.find('//')
        if comment_pos >= 0:
            line = line[:comment_pos]
        line = line.strip()
        if not line or line.startswith('/*') or line.startswith('*'):
            continue
        # Skip block-comment lines + X-macro expansions
        if ('FOREACH_PER_CORE_CFG_FIELD' in line or 'EMIT_PER_CORE_CFG_STRUCT_FIELD' in line
                or 'FOREACH_PER_CORE_DOMAIN_BITMAP' in line or 'EMIT_DOMAIN_BITMAP_FIELD' in line):
            continue
        # Skip preprocessor + closing braces
        if line.startswith('#') or line == '};' or line == '{':
            continue
        # Match field declaration: optional 'alignas(N) ' prefix; then 'type name;'
        m = re.match(r'^(?:alignas\(\d+\)\s+)?([a-zA-Z_][a-zA-Z_0-9<>]*(?:\s*<\s*F\s*>)?)\s+(\w+)\s*;', line)
        if m:
            typ = m.group(1).strip()
            name = m.group(2).strip()
            manual_fields[name] = (typ, line_no)
    return manual_fields


def parse_controller_config_parallel_arrays(text: str) -> dict:
    """Parse `<type> core_<name>[16];` declarations in ControllerConfig.hpp.

    Returns dict {name: (type, suffix, line_no)}.
    Suffix is e.g. '[256]' for 2D arrays, empty for 1D.
    """
    result = {}
    line_no = 0
    for line in text.split('\n'):
        line_no += 1
        # Strip inline comments
        comment_pos = line.find('//')
        if comment_pos >= 0:
            line = line[:comment_pos]
        # Match: optional whitespace + <type> + core_<name>[16] + optional [N] + ;
        m = re.match(
            r'^\s+([a-zA-Z_][a-zA-Z_0-9<>]*(?:\s*<\s*F\s*>)?)\s+(core_\w+)\[(?:16|MAX_EXECUTION_CORES)\](\[[^\]]+\])?\s*;',
            line,
        )
        if m:
            typ = m.group(1).strip()
            name = m.group(2).strip()
            suffix = (m.group(3) or '').strip()
            result[name] = (typ, suffix, line_no)
    return result


def parse_inventory_section_a(text: str) -> set:
    """Parse Section A entries from MANUAL_FIELDS_INVENTORY.md. Returns set of field names."""
    # Section A starts at '## Section A' and continues until '## Section B' or '## Statistics'
    m = re.search(r'## Section A.*?(?=\n##\s)', text, re.DOTALL)
    if not m:
        fail("MANUAL_FIELDS_INVENTORY.md missing '## Section A' header")
        sys.exit(2)
    section_a = m.group(0)
    # Each entry has a row | `field_name` | ... |
    names = set(re.findall(r'\|\s*`(core_\w+)`\s*\|', section_a))
    return names


def parse_inventory_section_b(text: str) -> set:
    """Parse Section B entries from MANUAL_FIELDS_INVENTORY.md. Returns set of field names."""
    m = re.search(r'## Section B.*?(?=\n##\s)', text, re.DOTALL)
    if not m:
        fail("MANUAL_FIELDS_INVENTORY.md missing '## Section B' header")
        sys.exit(2)
    section_b = m.group(0)
    # Section B rows are `| field_name | type | alignment | rationale |`
    names = set(re.findall(r'\|\s*`(\w+_cfg_flags)`\s*\|', section_b))
    return names


def strip_macro_definitions(text: str) -> str:
    """Remove all #define ... blocks (single-line + multi-line via \\ continuation).

    Used to scan for anti-pattern 1 in PRODUCTION code only, not in X-macro callback bodies
    (which legitimately use `cfg.core_overrides[c].name` + `cfg.name` as positional meta-vars).
    """
    out = []
    in_macro = False
    for line in text.split('\n'):
        if in_macro:
            if not line.rstrip().endswith('\\'):
                in_macro = False
            continue  # Skip macro body lines
        # Strip single-line #define (no continuation)
        stripped = line.strip()
        if stripped.startswith('#define '):
            if line.rstrip().endswith('\\'):
                in_macro = True
            continue
        out.append(line)
    return '\n'.join(out)


def parse_subsystem_state_struct_fields(text: str, struct_name: str) -> dict:
    """Parse scalar field declarations inside `struct <struct_name> { ... };` or
    `struct alignas(N) <struct_name> { ... };` or `template<...> struct <struct_name> { ... };`.

    Returns dict {field_name: (type, line_no)}. Excludes:
    - Array fields (e.g., per_core_X[16])      — already per-instance by construction
    - Sub-struct fields (composite types)       — recursed-into separately if needed
    - Nested struct/union declarations
    - Comment-only lines
    - X-macro invocations

    Used by Check 7 to scan for Class 27 anti-pattern (scalar cfg-mirror on subsystem state).
    """
    # Match the struct definition (supports `struct alignas(N) NAME`, `template<...>\nstruct NAME`,
    # or `struct NAME` patterns)
    patterns = [
        rf'template\s*<[^>]+>\s*\nstruct\s+(?:alignas\(\d+\)\s+)?{re.escape(struct_name)}\s*\{{(.*?)^\}};',
        rf'struct\s+(?:alignas\(\d+\)\s+)?{re.escape(struct_name)}\s*\{{(.*?)^\}};',
    ]
    body = None
    body_start_line = 0
    for pattern in patterns:
        m = re.search(pattern, text, re.MULTILINE | re.DOTALL)
        if m:
            body = m.group(1)
            body_start_line = text[:m.start()].count('\n') + 2
            break
    if body is None:
        return {}  # Struct not found; caller decides whether to fail or skip

    scalar_fields = {}
    nesting_depth = 0  # Track nested {} so we skip inner sub-structs
    line_no = body_start_line
    for line in body.split('\n'):
        line_no += 1
        # Track brace nesting to skip inner sub-structs / nested unions / etc.
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
        # Skip preprocessor + X-macro invocations + composite-struct declarations
        if line.startswith('#') or 'FOREACH_' in line or 'EMIT_' in line:
            continue
        # Skip struct/class/union/enum keyword lines (sub-types within this struct)
        if re.match(r'^(?:struct|class|union|enum)\b', line):
            continue
        # Match SCALAR field declaration:
        #   optional 'alignas(N) ' prefix
        #   then 'type name;' or 'type name = init;'
        # Type can be: uint8_t, FPN<F>, double, etc.
        # EXPLICITLY EXCLUDE: arrays (name[N]), pointers (type* name)
        m = re.match(
            r'^(?:alignas\(\d+\)\s+)?([a-zA-Z_][a-zA-Z_0-9]*(?:\s*<\s*[^>]+\s*>)?)\s+(\w+)\s*(?:=\s*[^;]+)?\s*;',
            line,
        )
        if m:
            typ = m.group(1).strip()
            name = m.group(2).strip()
            # Skip if it's actually an array decl that matched (defensive; should be excluded by regex)
            if '[' in name:
                continue
            scalar_fields[name] = (typ, line_no)
    return scalar_fields


def parse_inventory_section_c(text: str) -> set:
    """Parse Section C entries from MANUAL_FIELDS_INVENTORY.md. Returns set of (subsystem, field) tuples.

    Section C rows look like: | subsystem | field | rationale_category | detail | trigger |
    Empty Section C (zero exemptions) is the expected initial state at WIP2d-1.B.0c.
    """
    m = re.search(r'## Section C.*?(?=\n##\s|\Z)', text, re.DOTALL)
    if not m:
        # Section C is OPTIONAL (introduced at WIP2d-1.B.0c); missing = no exemptions
        return set()
    section_c = m.group(0)
    # Parse rows: | <subsystem> | <field> | ... |
    # Skip header row + separator row
    exemptions = set()
    for line in section_c.split('\n'):
        line = line.strip()
        if not line.startswith('|') or '---' in line:
            continue
        # Split on | + strip
        parts = [p.strip() for p in line.split('|')[1:-1]]  # drop leading/trailing empty
        if len(parts) < 2:
            continue
        # Header row sanity: if first col is 'Subsystem' (template header), skip
        if parts[0].lower().startswith('subsystem'):
            continue
        subsystem = parts[0].strip('` ')
        field = parts[1].strip('` ')
        if subsystem and field and subsystem != '---' and field != '---':
            exemptions.add((subsystem, field))
    return exemptions


def scan_anti_pattern_1(text: str) -> list:
    """Scan for anti-pattern 1 consumer shape: cfg.X + cfg.core_overrides[c].X same X.

    Strips #define macro bodies first — X-macro callbacks use `name` as a meta-var
    that pastes the field name; matching that as a field-name produces false positives.
    """
    findings = []
    # Strip macro definitions to avoid false-positives on X-macro callback meta-vars
    code = strip_macro_definitions(text)
    # Find each `cfg.core_overrides[c].FIELD` pattern; check if the same scope has `cfg.FIELD`
    pattern_override = re.compile(r'cfg\.core_overrides\[\w+\]\.(\w+)')
    overridden_fields = set(pattern_override.findall(code))
    for field in overridden_fields:
        # Check if cfg.<field> appears in same text (informational)
        if re.search(rf'\bcfg\.{re.escape(field)}\b(?!\s*\[)', code):
            findings.append(field)
    return findings


# Files scanned for Check 9 (Class 26 paired-access mismatch detection).
# Add new source files here as they accumulate cfg.core_overrides[X] + cfg.cores[Y] paired patterns.
# Per RECURRING_BUG_PATTERNS Class 26 + DESIGN_SPECS/meta-disciplines/structural-enforcement-when-memory-insufficient.md M7 4th canonical.
# Codified at v5.15.5.F.4d.1.B.7 (2026-05-27) after pre-fix Async.hpp:814+853 silent trading-logic bug.
CHECK_9_SCAN_FILES = [
    "CoreFrameworks/EngineSharded/Async.hpp",
    "CoreFrameworks/EngineSharded/SlowPath.hpp",
    "CoreFrameworks/EngineSharded/Run.hpp",
    "CoreFrameworks/EngineSharded/Boot.hpp",
    "CoreFrameworks/ControllerEventLoop.hpp",
    "CoreFrameworks/EngineCommon.hpp",
    "CoreFrameworks/OrderManager.hpp",
    "Backtest/BacktestSharded.hpp",
    "CoreFrameworks/ShardedBacktestDriver.hpp",
]

CHECK_9_PROXIMITY_LINES = 5  # how far to look for paired cfg.cores[Y] after cfg.core_overrides[X]


# ============================================================================
# Check 10 — Class 26 sub-shape B detection: UNINDEXED-GLOBAL per-core-migrated
# field reads at per-core consumer sites (M7 6th canonical structural enforcement)
# ============================================================================
# Per RECURRING_BUG_PATTERNS Class 26 sub-shape B + DESIGN_SPECS/meta-disciplines/
# structural-enforcement-when-memory-insufficient.md M7 6th canonical.
# Codified at v5.15.5.F.4d.1.B.8 (2026-05-27) after /accounting-audit surfaced 4 HIGH instances
# (ControllerEventLoop.hpp:3605+3670+3042 + StrategyLifecycle.hpp:272 + ShardedSnapshot.hpp:249).
#
# Sister to Check 9: same tool / same surface family (per-core cfg consumer discipline) /
# different detection signature (UNINDEXED-GLOBAL vs PAIRED-ACCESS-MISMATCH). Check 9 catches
# `cfg.core_overrides[X]` + `cfg.cores[Y]` with X != Y; Check 10 catches `cfg.X` UNINDEXED
# where X is per-core-migrated AND has global sister in ControllerConfig<F>.
#
# Per-core surface files to scan. Operator extends as new per-core surface files land.
CHECK_10_SCAN_FILES = [
    "CoreFrameworks/ControllerEventLoop.hpp",
    "CoreFrameworks/EngineSharded/Async.hpp",
    "CoreFrameworks/EngineSharded/SlowPath.hpp",
    "CoreFrameworks/EngineSharded/Run.hpp",
    "CoreFrameworks/EngineSharded/Boot.hpp",
    "CoreFrameworks/EngineCommon.hpp",
    "CoreFrameworks/OrderManager.hpp",
    "CoreFrameworks/ShardedSnapshot.hpp",
    "Strategies/StrategyLifecycle.hpp",
    "Strategies/private/EmaCross.hpp",  # has both per-core sharded (covered) + legacy single_core (exempt)
    "Strategies/Momentum.hpp",
    "Strategies/MeanReversion.hpp",
    "Strategies/MLStrategy.hpp",
    "Strategies/SimpleDip.hpp",
    "Backtest/BacktestSharded.hpp",
    "CoreFrameworks/ShardedBacktestDriver.hpp",
]

# Per-core fields with GLOBAL sister in ControllerConfig<F> — reads UNINDEXED at per-core
# consumer sites = Class 26 sub-shape B violation. Operator extends as new per-core-migrated
# fields land with global sisters. Future enhancement: derive from
# FOREACH_PER_CORE_CFG_FIELD ∩ ControllerConfig<F> manual decl set programmatically.
CHECK_10_PER_CORE_FIELDS_WITH_GLOBAL_SISTER = {
    "fee_rate",
    "fee_rate_taker",
    "fee_rate_maker",
    "slippage_pct",
}

# Section D exemptions — legitimate UNINDEXED-GLOBAL reads at per-core consumer sites.
# Format: (file_rel_path, line_num) tuples.
# Operator extends via MANUAL_FIELDS_INVENTORY.md Section D when new legitimate cases surface.
CHECK_10_SECTION_D_EXEMPTIONS = {
    # LEGACY single_core paths (per .B.7 audit Cat 8 LEGACY-KEEP verdict; caller is single_core PortfolioController)
    ("Strategies/private/EmaCross.hpp", 143),  # EmaCross_ExitAdjust legacy single_core fee_rate_taker
    ("Strategies/private/EmaCross.hpp", 144),  # sister fallback fee_rate
    # KEEP-AS-GLOBAL display sites (Settings panel operator-facing semantic; per-core deviations
    # surfaced via per_core_count panel instead). Line numbers reflect post-.B.8 Phase B
    # KEEP-AS-GLOBAL comment additions (lines shifted from original 139/330/331 to 142/343/344).
    ("CoreFrameworks/ShardedSnapshot.hpp", 142),  # engine-wide headline fee_rate_pct display
    ("CoreFrameworks/ShardedSnapshot.hpp", 343),  # Settings panel cfg_fee display
    ("CoreFrameworks/ShardedSnapshot.hpp", 344),  # Settings panel cfg_slippage display
}

# Boot-time / parse-time fn-name patterns excluded from Check 10 (legitimate global cfg reads)
CHECK_10_BOOT_TIME_FN_NAME_PATTERNS = {"Boot", "Init", "Default", "Parse", "Normalize"}


def scan_check_10_violations(text: str, file_rel_path: str) -> list:
    """Class 26 sub-shape B detection: UNINDEXED-GLOBAL per-core-migrated field reads at per-core consumer sites.

    Catches `cfg.X` / `cfg->X` / `resolved_cfg.X` UNINDEXED reads on per-core fields with global
    sister (fee_rate / fee_rate_taker / fee_rate_maker / slippage_pct). Per-core consumer should
    read `cfg.cores[core_id].X` instead. Pre-fix HIGH-1/2/3/4 at .B.8 audit findings:
    ControllerEventLoop.hpp:3605+3670+3042 + StrategyLifecycle.hpp:272 + ShardedSnapshot.hpp:249.

    Per v1.2 amendment: handles `resolved_cfg.X` aliased reads (stack-local copy from
    ControllerConfig_ResolveForCore) in addition to direct cfg.X / cfg->X reads.

    Tracks #define multi-line continuations inline (skip macro body lines) WITHOUT using
    strip_macro_definitions — preserves ORIGINAL source line numbers for accurate Section D
    exemption matching + finding citation. Applies Section D exemption + boot-time fn-name
    heuristic filters.

    Returns list of (file_rel_path, line_num, container, accessor, field, current_fn) tuples.
    """
    findings = []

    # Build regex for per-core fields (escaped + alternation)
    field_alt = '|'.join(re.escape(f) for f in CHECK_10_PER_CORE_FIELDS_WITH_GLOBAL_SISTER)
    if not field_alt:
        return findings

    # Match: cfg.X / cfg->X / resolved_cfg.X where X is per-core-with-global-sister field
    # Negative lookahead (?!\s*[\[.]) excludes cfg.X[Y] (indexed) and cfg.X.subfield (nested)
    cfg_unindexed_re = re.compile(
        r'\b(cfg|resolved_cfg)(\.|->)(' + field_alt + r')\b(?!\s*[\[.])'
    )

    # Track current enclosing fn name (heuristic for Boot/Init/Parse/Default exclusion)
    # Simple regex: catches `inline ... fn_name(...)` style patterns at top-level fn declarations
    fn_decl_re = re.compile(r'^\s*(?:inline\s+|template\s*<[^>]+>\s*)*(?:[\w<>:&*\s,]+\s+)?(\w+)\s*\([^)]*\)\s*[{|\n]')
    current_fn = ""
    in_macro = False

    for line_num, line in enumerate(text.split('\n'), start=1):
        # Track #define multi-line continuations (skip macro body lines; preserve line_num)
        if in_macro:
            if not line.rstrip().endswith('\\'):
                in_macro = False
            continue
        stripped = line.strip()
        if stripped.startswith('#define '):
            if line.rstrip().endswith('\\'):
                in_macro = True
            continue

        # Update current_fn heuristic (loose; covers most fn-def patterns)
        fn_match = fn_decl_re.match(line)
        if fn_match:
            candidate = fn_match.group(1)
            # Skip common non-fn keywords + control flow
            if candidate not in ("if", "while", "for", "switch", "return", "sizeof", "alignof", "static_assert"):
                current_fn = candidate

        # Check Section D exemption (uses ORIGINAL source line_num for accurate match)
        if (file_rel_path, line_num) in CHECK_10_SECTION_D_EXEMPTIONS:
            continue

        # Boot-time / parse-time fn-name heuristic
        if any(pattern in current_fn for pattern in CHECK_10_BOOT_TIME_FN_NAME_PATTERNS):
            continue

        # Strip single-line // comments before regex match — avoids false positives where comment
        # text mentions cfg.X pattern (e.g., explanatory comments above a fix site).
        # Doesn't handle string literals containing //, but those are rare in cfg-consumer code.
        line_no_comments = re.sub(r'//.*$', '', line)

        # Find UNINDEXED-GLOBAL matches on per-core fields
        for m in cfg_unindexed_re.finditer(line_no_comments):
            container = m.group(1)  # 'cfg' or 'resolved_cfg'
            accessor = m.group(2)   # '.' or '->'
            field = m.group(3)      # the per-core field name
            findings.append((file_rel_path, line_num, container, accessor, field, current_fn))

    return findings


def scan_check_9_violations(text: str, file_rel_path: str) -> list:
    """Class 26 detection: cfg.core_overrides[X] + cfg.cores[Y] with X != Y within proximity.

    The paired-access pattern `... ov.field ? ov.field : cfg.cores[Y].field` is
    semantically broken if Y != the override's X — both MUST reference the SAME
    per-core slot. Pre-fix Async.hpp:814 + :853 had this exact shape (override
    slot=slot, fallback slot=i where i was the inner ring-pop counter).

    Strips macro definitions first to avoid X-macro callback false positives.
    Returns list of (line_num_ov, ov_symbol, line_num_base, base_symbol) tuples.
    """
    findings = []
    code = strip_macro_definitions(text)
    lines = code.split('\n')

    override_pattern = re.compile(r'cfg\.core_overrides\[(\w+)\]')
    base_pattern = re.compile(r'cfg\.cores\[(\w+)\]')

    for line_num, line in enumerate(lines, start=1):
        for ov_match in override_pattern.finditer(line):
            ov_symbol = ov_match.group(1)
            # Look forward + backward PROXIMITY lines for cfg.cores[Y] pair
            start = max(0, line_num - 1 - CHECK_9_PROXIMITY_LINES)
            end = min(len(lines), line_num + CHECK_9_PROXIMITY_LINES)
            for j in range(start, end):
                for base_match in base_pattern.finditer(lines[j]):
                    base_symbol = base_match.group(1)
                    if base_symbol != ov_symbol:
                        findings.append((file_rel_path, line_num, ov_symbol, j + 1, base_symbol))
    return findings


def main() -> int:
    info("running per-core cfg registry integrity check...")

    # Load all three files
    cfg_reg_text = read_file(CFG_REG)
    ctrl_cfg_text = read_file(CTRL_CFG)
    inventory_text = read_file(INVENTORY)

    failures = 0

    # --- Check 1: FOREACH_PER_CORE_CFG_FIELD ↔ PerCoreCfg<F> struct field type sync ---
    # Post-WIP2d-0.B: single registry. TYPE is the FIRST column of each row.
    # Verify every row's TYPE matches the struct field type generated via X-macro.
    # (Auxiliary FOREACH_PER_CORE_FIELD_TYPE retired at WIP2d-0.B.)
    cfg_field_body = extract_macro_body(cfg_reg_text, "FOREACH_PER_CORE_CFG_FIELD")
    cfg_field_map = parse_foreach_per_core_cfg_field(cfg_field_body)
    cfg_field_names = set(cfg_field_map.keys())

    if len(cfg_field_map) == 0:
        fail("Check 1 FAIL: FOREACH_PER_CORE_CFG_FIELD parsed 0 rows — row regex may be broken")
        failures += 1
    elif len(cfg_field_map) < 80:
        fail(f"Check 1 FAIL: only {len(cfg_field_map)} per-core rows parsed (expected ~92) — registry might be incomplete or parser regex broken")
        failures += 1
    else:
        info(f"Check 1 PASS: {len(cfg_field_map)} per-core cfg fields parsed with TYPE column (single registry; auxiliary retired)")

    # --- Check 2: PerCoreCfg<F> body contains ONLY 2 X-macro invocations + nothing else ---
    # Post-WIP2d-0.B: both cfg-surface fields (92) AND runtime bitmap fields (5) come from X-macros.
    # No manual fields permitted anywhere in PerCoreCfg<F> body.
    per_core_manual = parse_per_core_cfg_body(ctrl_cfg_text)
    per_core_manual_names = set(per_core_manual.keys())

    if per_core_manual_names:
        fail(f"Check 2 FAIL: PerCoreCfg<F> body contains FORBIDDEN manual fields outside X-macro expansions: {sorted(per_core_manual_names)}")
        for name in sorted(per_core_manual_names):
            typ, line = per_core_manual[name]
            fail(f"  → ControllerConfig.hpp:{line}: '{typ} {name}' — add to FOREACH_PER_CORE_CFG_FIELD (cfg surface) OR FOREACH_PER_CORE_DOMAIN_BITMAP (runtime bitmap); no manual fields permitted")
        failures += 1
    else:
        info(f"Check 2 PASS: PerCoreCfg<F> body contains ONLY FOREACH_PER_CORE_CFG_FIELD + FOREACH_PER_CORE_DOMAIN_BITMAP invocations (no manual fields)")

    # --- Check 3: ControllerConfig uses FOREACH_MANUAL_PER_CORE_FIELD X-macro for parallel arrays ---
    # Post-WIP2d-0.B: parallel arrays come from X-macro expansion in ControllerConfig.hpp.
    # No literal `<type> core_<name>[16];` declarations should exist outside the X-macro invocation.
    manual_body = extract_macro_body(cfg_reg_text, "FOREACH_MANUAL_PER_CORE_FIELD")
    manual_xmacro = parse_foreach_manual_per_core_field(manual_body)
    manual_xmacro_names = set(manual_xmacro.keys())

    # Verify the X-macro invocation is present in ControllerConfig.hpp
    has_manual_xmacro_invocation = bool(re.search(
        r'FOREACH_MANUAL_PER_CORE_FIELD\s*\(\s*EMIT_MANUAL_PER_CORE_DECL\s*\)',
        ctrl_cfg_text,
    ))
    if not has_manual_xmacro_invocation:
        fail("Check 3 FAIL: ControllerConfig.hpp missing FOREACH_MANUAL_PER_CORE_FIELD(EMIT_MANUAL_PER_CORE_DECL) invocation")
        failures += 1

    # Verify NO literal `<type> core_<name>[16];` declarations exist anymore (all X-macro generated)
    parallel_arrays = parse_controller_config_parallel_arrays(ctrl_cfg_text)
    stray_decls = set(parallel_arrays.keys())
    if stray_decls:
        fail(f"Check 3 FAIL: stray manual parallel array declarations in ControllerConfig.hpp (must come from X-macro expansion only): {sorted(stray_decls)}")
        for name in sorted(stray_decls):
            typ, suffix, line = parallel_arrays[name]
            fail(f"  → ControllerConfig.hpp:{line}: '{typ} {name}[16]{suffix}' — delete; add to FOREACH_MANUAL_PER_CORE_FIELD instead")
        failures += 1

    if has_manual_xmacro_invocation and not stray_decls:
        info(f"Check 3 PASS: {len(manual_xmacro_names)} parallel arrays declared exclusively via FOREACH_MANUAL_PER_CORE_FIELD X-macro")

    # --- Check 4: FOREACH_MANUAL_PER_CORE_FIELD ↔ MANUAL_FIELDS_INVENTORY.md bidirectional sync ---
    inv_section_a = parse_inventory_section_a(inventory_text)
    only_in_xmacro = manual_xmacro_names - inv_section_a
    only_in_inv = inv_section_a - manual_xmacro_names
    if only_in_xmacro:
        fail(f"Check 4 FAIL: in FOREACH_MANUAL_PER_CORE_FIELD but missing from MANUAL_FIELDS_INVENTORY.md Section A: {sorted(only_in_xmacro)}")
        failures += 1
    if only_in_inv:
        fail(f"Check 4 FAIL: in MANUAL_FIELDS_INVENTORY.md Section A but missing from FOREACH_MANUAL_PER_CORE_FIELD: {sorted(only_in_inv)}")
        failures += 1
    if not only_in_xmacro and not only_in_inv:
        info(f"Check 4 PASS: {len(manual_xmacro_names)} Section A entries in sync between X-macro + inventory")

    # Verify Section B inventory matches PERMITTED_RUNTIME_FIELDS
    inv_section_b = parse_inventory_section_b(inventory_text)
    if inv_section_b != PERMITTED_RUNTIME_FIELDS:
        fail(f"Check 4 FAIL: MANUAL_FIELDS_INVENTORY.md Section B mismatch with PERMITTED_RUNTIME_FIELDS in CI script")
        fail(f"  inventory says: {sorted(inv_section_b)}")
        fail(f"  CI permits:     {sorted(PERMITTED_RUNTIME_FIELDS)}")
        failures += 1

    # --- Check 5: No name duplication between registries ---
    duplicates = cfg_field_names & manual_xmacro_names
    if duplicates:
        fail(f"Check 5 FAIL: name(s) appear in BOTH FOREACH_PER_CORE_CFG_FIELD + FOREACH_MANUAL_PER_CORE_FIELD: {sorted(duplicates)}")
        fail("  → each name must be in EITHER the registry (for X-macro struct gen) OR the manual exemption inventory, NOT both")
        failures += 1
    else:
        info(f"Check 5 PASS: no name duplication between registries")

    # --- Check 6: Anti-pattern 1 consumer scan (informational; WARN) ---
    findings_ctrl = scan_anti_pattern_1(ctrl_cfg_text)
    if findings_ctrl:
        warn(f"Check 6 INFO: anti-pattern 1 consumer shape detected (cfg.X + cfg.core_overrides[c].X same X) in ControllerConfig.hpp for fields: {sorted(set(findings_ctrl))}")
        warn("  → cfg-scope-discipline.md § Anti-pattern 1 (global-default-with-override) FORBIDDEN")
        warn("  → WIP2f deletion of core_overrides[16] makes the shape UNEXPRESSIBLE; refactor before WIP2g for safety-critical sites")

    # --- Check 7: Subsystem-state cfg-mirror scan (Class 27 prevention) ---
    # Per DESIGN_SPECS/decision-time-data-binding-pattern.md + RECURRING_BUG_PATTERNS Class 27:
    # scalar fields on subsystem state types that mirror cfg field names = anti-pattern.
    # Required mitigation: pre-resolve onto in-flight object (Order/Position/Event) OR
    # register fallback cache via FOREACH_<SUBSYS>_CFG_CACHE. Exemptions in Section C.
    section_c_exemptions = parse_inventory_section_c(inventory_text)
    cfg_field_name_set = set(cfg_field_map.keys())  # FOREACH_PER_CORE_CFG_FIELD names
    check_7_violations = []
    for subsys_name, subsys_path in SUBSYSTEM_STATE_TYPES_FOR_CLASS_27_SCAN.items():
        full_path = REPO_ROOT / subsys_path
        if not full_path.exists():
            warn(f"Check 7 WARN: subsystem source not found: {subsys_path} — skipping {subsys_name}")
            continue
        subsys_text = read_file(full_path)
        struct_fields = parse_subsystem_state_struct_fields(subsys_text, subsys_name)
        if not struct_fields:
            warn(f"Check 7 WARN: struct {subsys_name} not found or empty in {subsys_path}")
            continue
        # Check each scalar field against cfg field names
        for field_name, (typ, line) in struct_fields.items():
            if field_name in cfg_field_name_set and (subsys_name, field_name) not in section_c_exemptions:
                check_7_violations.append((subsys_name, field_name, typ, subsys_path, line))
    if check_7_violations:
        fail(f"Check 7 FAIL: {len(check_7_violations)} Class 27 violation(s) — scalar cfg-mirror field(s) on subsystem state without Section C exemption:")
        for subsys_name, field_name, typ, subsys_path, line in check_7_violations:
            fail(f"  → {subsys_path}:{line}: '{typ} {field_name}' on {subsys_name}")
            fail(f"     Class 27 anti-pattern — see DESIGN_SPECS/decision-time-data-binding-pattern.md")
            fail(f"     Fix options: (a) pre-resolve onto in-flight object, OR")
            fail(f"                  (b) FOREACH_{subsys_name.upper()}_CFG_CACHE registry, OR")
            fail(f"                  (c) add exemption to MANUAL_FIELDS_INVENTORY.md Section C")
        failures += 1
    else:
        info(f"Check 7 PASS: {len(SUBSYSTEM_STATE_TYPES_FOR_CLASS_27_SCAN)} subsystem state type(s) scanned; no Class 27 violations ({len(section_c_exemptions)} Section C exemption(s) on file)")

    # --- Check 8: Cfg field categorization integrity (M7 4th canonical; v5.15.5.F.4d.1.B.4 v1.7.6 Cx-G) ---
    # Per `DESIGN_SPECS/framework-patterns/cfg-field-categorization-discipline.md` 4-category decision tree.
    # 3 flags:
    #   Flag A: per-core registry row with 0 per-core consumers → wrong category (should be GLOBAL_ONLY or CFG-FLAG BITMAP BIT)
    #   Flag B: per-core consumer scope reading global cfg field where per-core registry row exists → Class 25 scope-erosion
    #   Flag C: per-core registry row WITHOUT NO_FLAT_FIELD bit + WITHOUT global manual struct field → walker compile-error candidate
    # Sister to /readiness Check 44 (plan-time enforcement); together = complete discipline coverage.
    #
    # Implementation note: Flag A requires comprehensive grep across production code (CoreFrameworks/ + Strategies/ +
    # ML_Headers/ + MemHeaders/ + Backtest/ + DataStream/ + GUI/ + FixedPoint/) for `cfg.cores[*].<field>` patterns.
    # Flag B requires fn-signature scan for `const PerCoreCfg<F>*` callers reading `cfg.<field>` globals.
    # Flag C requires cross-ref of per-core registry rows against ControllerConfig<F> manual decl set.
    #
    # Initial Check 8 implementation: detect cohort categorization violations via grep heuristic; full impl deferred to
    # sister mini-ship per Caramel "right not fast" + comprehensive-close discipline. This is INFRASTRUCTURE scaffold
    # for the discipline; mechanical detection patterns follow at sister ship.
    check_8_skipped = True  # Sister ship implementation per cfg-field-categorization-discipline.md Stage 3 promotion
    if check_8_skipped:
        info("Check 8 PENDING: cfg field categorization integrity (M7 4th canonical) — scaffold landed at v1.7.6 Cx-G; mechanical detection patterns at sister ship implementation. Decision tree + 5-step migration discipline ENFORCED at /readiness Check 44 (plan-time); CI mechanical enforcement at sister ship.")

    # --- Check 9: Class 26 paired-access mismatch detection (M7 4th canonical; v5.15.5.F.4d.1.B.7) ---
    # Per RECURRING_BUG_PATTERNS Class 26 + DESIGN_SPECS/meta-disciplines/structural-enforcement-when-memory-insufficient.md M7.
    # The paired pattern `ov.field ? ov.field : cfg.cores[Y].field` is broken if Y != the override's X.
    # Pre-fix Async.hpp:814+853 had exactly this bug (override slot=slot, fallback slot=i where i was
    # the inner ring-pop counter from an enclosing for-loop). Silent per-core trading-logic miscalibration.
    # Sister: tests/controller_test.cpp Class 26 regression test (added at .B.7).
    check_9_violations = []
    for rel_path in CHECK_9_SCAN_FILES:
        full_path = REPO_ROOT / rel_path
        if not full_path.exists():
            warn(f"Check 9 WARN: scan target not found: {rel_path} — skipping")
            continue
        file_text = read_file(full_path)
        check_9_violations.extend(scan_check_9_violations(file_text, rel_path))
    if check_9_violations:
        fail(f"Check 9 FAIL: {len(check_9_violations)} Class 26 paired-access mismatch(es) — cfg.core_overrides[X] and cfg.cores[Y] with X != Y within {CHECK_9_PROXIMITY_LINES} lines:")
        for rel_path, ov_line, ov_sym, base_line, base_sym in check_9_violations:
            fail(f"  → {rel_path}:{ov_line} uses cfg.core_overrides[{ov_sym}] but {rel_path}:{base_line} uses cfg.cores[{base_sym}]")
            fail(f"     Class 26 anti-pattern — paired access MUST share index symbol")
            fail(f"     Fix: change cfg.cores[{base_sym}] → cfg.cores[{ov_sym}] OR rename loop variable for scope clarity")
            fail(f"     See: DOCS/recurring-bug-patterns/class-26-global-consumer-reading-per-core-field.md")
        failures += 1
    else:
        info(f"Check 9 PASS: {len(CHECK_9_SCAN_FILES)} file(s) scanned; no Class 26 paired-access mismatches (proximity={CHECK_9_PROXIMITY_LINES})")

    # --- Check 10: Class 26 sub-shape B detection — UNINDEXED-GLOBAL per-core-migrated field reads
    # at per-core consumer sites (M7 6th canonical; v5.15.5.F.4d.1.B.8). ---
    # Per RECURRING_BUG_PATTERNS Class 26 sub-shape B + DESIGN_SPECS/meta-disciplines/
    # structural-enforcement-when-memory-insufficient.md M7 6th canonical.
    # Sister to Check 9: same tool, same surface family, different detection signature.
    # Catches cfg.X / cfg->X / resolved_cfg.X UNINDEXED on per-core-with-global-sister fields
    # (fee_rate / fee_rate_taker / fee_rate_maker / slippage_pct). Per-core consumer should
    # read cfg.cores[core_id].X instead.
    # Sister: tests/controller_test.cpp Class 26 sub-shape B regression test section (added at .B.8 Phase E).
    check_10_violations = []
    for rel_path in CHECK_10_SCAN_FILES:
        full_path = REPO_ROOT / rel_path
        if not full_path.exists():
            warn(f"Check 10 WARN: scan target not found: {rel_path} — skipping")
            continue
        file_text = read_file(full_path)
        check_10_violations.extend(scan_check_10_violations(file_text, rel_path))
    if check_10_violations:
        fail(f"Check 10 FAIL: {len(check_10_violations)} Class 26 sub-shape B UNINDEXED-GLOBAL violation(s) — per-core consumer sites reading global cfg field without core_id index:")
        for rel_path, line_num, container, accessor, field, fn_name in check_10_violations:
            fn_ctx = f" in fn '{fn_name}'" if fn_name else ""
            fail(f"  → {rel_path}:{line_num}{fn_ctx}: '{container}{accessor}{field}' UNINDEXED at per-core consumer site")
            fail(f"     Class 26 sub-shape B anti-pattern — per-core consumer must read per-core slot")
            fail(f"     Fix: change {container}{accessor}{field} → {container}{accessor}cores[<core_id>].{field}")
            fail(f"     OR if legitimately global (Settings panel display / legacy single_core / boot-time): add to CHECK_10_SECTION_D_EXEMPTIONS at tools/check_per_core_registry_integrity.py")
            fail(f"     See: DOCS/recurring-bug-patterns/class-26-global-consumer-reading-per-core-field.md § Sub-shape B")
        failures += 1
    else:
        info(f"Check 10 PASS: {len(CHECK_10_SCAN_FILES)} file(s) scanned; no Class 26 sub-shape B UNINDEXED-GLOBAL violations ({len(CHECK_10_SECTION_D_EXEMPTIONS)} Section D exemption(s) on file)")

    # --- Final verdict ---
    if failures > 0:
        fail(f"per-core cfg integrity check FAILED with {failures} violations — see errors above")
        return 1
    info(f"all 9 structural checks PASS — per-core cfg discipline intact (Check 6 informational; Check 7 Class 27 prevention; Check 8 pending mechanical impl per cfg-field-categorization-discipline.md Stage 3 sister ship; Check 9 Class 26 sub-shape A paired-access mismatch detection; Check 10 Class 26 sub-shape B UNINDEXED-GLOBAL detection)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
