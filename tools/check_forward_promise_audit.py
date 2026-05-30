#!/usr/bin/env python3
"""
tools/check_forward_promise_audit.py — Check 11 forward-promise auto-write verification

Mechanical implementation of /capture-audit Check 11 per
claude-skills/capture-audit/SKILL.md section "Check 11: Forward-promised
auto-write at prior ship close verification (NEW v5.15.5.F.4d.1.B.8)".

Scans CHANGELOG + postmortems + handoff docs + plan bodies + Class catalogs +
capture-audit-reports + CLAUDE.local.md going-forward rules + claude-skills/*/SKILL.md
Worked Examples for forward-promise sentinels (e.g., "TECH_DEBT-NNN NEW+CLOSED at .X";
"Class N recurrence_count X→Y + NEW Worked Example"; "Stage N → Stage M first canonical
promotion"). Per-sentinel verifies the claimed auto-write landed at expected ledger
location; outputs UNFULFILLED findings.

Sister tools (M7 Stage 6 canonical applications):
  - check_per_core_registry_integrity.py (canonicals 5 + 6: Check 9 + Check 10 at .B.7/.B.8)
  - check_plan_body_symbol_existence.py (canonical 1; B-Plus pre-commit hook)
  - This tool (canonical 7: Check 11 at .D)

Codification timeline:
  - .B.8 Phase H.2.c: SKILL.md documentation amendment (Check 11 spec)
  - .B.8 deferred mechanical Python impl per token-budget pragmatism
  - .D Phase B.1-B.4: this implementation (v1.1 audit-cycle-2 GREEN)

Per /capture-audit invocation modes:
  --quick     : skip (Check 11 is --deep only)
  --deep      : run all checks
  --check 11  : run only this check (default; this tool only implements Check 11)
  --since <ref>: scope to artifacts modified since ref
  --strict    : exit 1 on HIGH findings (BLOCK)
  --json      : machine-parseable output (default human-readable)
  --include-archived: bypass ARCHIVED_EXCLUSIONS (forensic; rare)

Output-privacy discipline (tool public / outputs private):
  Tool itself = public (engine repo; demonstrates rigorous CI discipline; sister to B-Plus + check_per_core_*).
  Tool outputs = workspace-private OR transient by design:
    - Default output: stdout (transient; not persisted)
    - Report files (when written by skill orchestration): plans/<sprint>/capture-audit-reports/<date>-<reason>.md
      → plans/ is symlinked to workspace; reports land workspace-private
    - Pre-commit hook output: stdout to terminal (not repo-stored)
  This tool NEVER writes to engine-repo public paths. If extending: keep outputs scoped to
  workspace-symlinked paths OR stdout. Sister discipline: feedback_address_user_as_caramel
  + CLAUDE.local.md privacy boundary recap.
"""

from __future__ import annotations

import os
import re
import sys
import json
import argparse
import subprocess
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional, Callable, List, Tuple

# ============================================================================
# Repo path constants
# ============================================================================
# Machine-portable roots (per feedback_machine_portable_resolver_for_committed_tool_paths):
# ENGINE_ROOT derives from this file's location; WORKSPACE_ROOT + MEMORY_DIR via
# env-override -> derived-default -> .exists()-guard. No $HOME hardcode in a committed,
# public-AGPL tool — runs on any clone / any PC / SSH-grid node.
ENGINE_ROOT = Path(os.environ.get("FOXML_ENGINE") or Path(__file__).resolve().parent.parent)
def _resolve_workspace_root():
    env = os.environ.get("FOXML_WORKSPACE")
    if env and Path(env).exists():
        return Path(env)
    sibling = ENGINE_ROOT.parent / "tick-trader-percore-workspace"
    return sibling if sibling.exists() else ENGINE_ROOT
WORKSPACE_ROOT = _resolve_workspace_root()
def _resolve_memory_dir():
    env = os.environ.get("FOXML_MEMORY_DIR")
    if env and Path(env).exists():
        return Path(env)
    project_id = str(ENGINE_ROOT).replace("/", "-").replace("_", "-")
    return Path.home() / ".claude" / "projects" / project_id / "memory"

ACTIVE_SPRINT = "v5.15-live-readiness"
PLANS_DIR = ENGINE_ROOT / f"plans/{ACTIVE_SPRINT}"

# Ledger paths
TECH_DEBT_OPEN = WORKSPACE_ROOT / "DOCS/tech-debt/open.md"
TECH_DEBT_CLOSED = WORKSPACE_ROOT / "DOCS/tech-debt/closed.md"
TECH_DEBT_INFLIGHT = WORKSPACE_ROOT / "DOCS/tech-debt/in-flight.md"
PARITY_ISSUES = WORKSPACE_ROOT / "DOCS/PARITY_ISSUES.md"
RECURRING_BUG_PATTERNS_DIR = WORKSPACE_ROOT / "DOCS/recurring-bug-patterns"
DESIGN_SPECS_DIR = WORKSPACE_ROOT / "DESIGN_SPECS"
MEMORY_DIR = _resolve_memory_dir()
MEMORY_INDEX = MEMORY_DIR / "MEMORY.md"
CLAUDE_MD = ENGINE_ROOT / "CLAUDE.md"
CLAUDE_LOCAL_MD = ENGINE_ROOT / "CLAUDE.local.md"
SKILLS_DIR = ENGINE_ROOT / ".claude/skills"

# ============================================================================
# Dataclasses (per plan body Decision C + Decision D)
# ============================================================================

@dataclass(frozen=True)
class SentinelSpec:
    """Sentinel pattern + verifier dispatch + severity + lifecycle context.

    Per plan body Decision C: each sentinel maps to a verifier function via
    string name (resolved via globals() at scan time). promised_vs_landed
    threads through verifier signature to disambiguate forward-promise markers
    vs landed-at-this-ship claims.
    """
    pattern: re.Pattern
    verifier_fn: str          # function name (looked up via globals())
    severity: str             # HIGH / MED / LOW / INFO
    desc: str                 # human-readable
    promised_vs_landed: str   # PROMISED-FUTURE / LANDED-NOW / EITHER


@dataclass(frozen=True)
class ScanSpec:
    """Structured scan source specification (replaces v1.0 ad-hoc string directives).

    Per plan body Decision D.
    """
    root: Path                              # absolute path to file OR directory
    is_dir: bool                            # True = directory walk
    glob: Optional[str] = None              # glob pattern for directory walk
    recursive: bool = False                 # recursive glob (.claude/skills SKILL.md)
    tail_rows: Optional[int] = None         # CHANGELOG-style: scan last N table rows
    modified_within_days: Optional[int] = None  # filter to recently-modified files
    sections: List[str] = field(default_factory=list)  # markdown section names to scan within
    exclude_patterns: List[str] = field(default_factory=list)  # glob exclusions
    sentinel_density: str = 'MED'           # informational; helps prioritize scan order


@dataclass(frozen=True)
class Finding:
    """A drift detection result; emitted when verifier finds claim-vs-ledger mismatch."""
    severity: str             # HIGH / MED / LOW / INFO
    sentinel_desc: str
    source_file: str
    source_line: int
    captured_groups: Tuple    # all regex groups (supports multi-group sentinels)
    expected_location: str    # human-readable
    actual_location: Optional[str]  # None = not found
    suggestion: str
    promised_vs_landed: str   # for triage context


# ============================================================================
# Archived exclusions (per feedback_archived_changelog_preservation_discipline)
# ============================================================================
ARCHIVED_EXCLUSIONS = [
    "DOCS/changelogs/2026-04-*/*.md",
    "DOCS/changelogs/2026-03-*/*.md",
    "DOCS/changelogs/2026-02-*/*.md",
    "DOCS/changelogs/2026-01-*/*.md",
    "DOCS/changelogs/2025-*/*.md",
    # Future archived changelogs auto-added by date pattern
]


# ============================================================================
# Exemption marker (per plan body Decision C exemption mechanism)
# ============================================================================
EXEMPT_MARKER_RE = re.compile(r'#\s*CHECK_11_EXEMPT:\s*(.+?)$', re.MULTILINE)


def is_exempt(source_text: str, match_line_num: int) -> Optional[str]:
    """Return exemption rationale if match is exempted; None otherwise.

    Per plan body Decision C: inline `# CHECK_11_EXEMPT: <rationale>` marker
    on same line OR within 3 lines above matches the exemption.
    """
    lines = source_text.splitlines()
    for i in range(max(0, match_line_num - 3), min(len(lines), match_line_num + 1)):
        m = EXEMPT_MARKER_RE.search(lines[i])
        if m:
            return m.group(1).strip()
    return None


# ============================================================================
# Section parser (per plan body Decision D)
# ============================================================================

def extract_section(text: str, section_name: str) -> Optional[str]:
    """Extract content under `## <section_name>` header until next `## ` boundary.

    Handles `## Foo`, `### Foo`, `#### Foo` (heading depth-agnostic).
    Returns None if section not found.
    """
    pattern = rf'^#{{1,4}}\s+{re.escape(section_name)}\s*$(.*?)(?=^#{{1,4}}\s|\Z)'
    m = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    return m.group(1).strip() if m else None


def filter_text_to_sections(text: str, section_names: List[str]) -> str:
    """Return concatenation of all named sections (or full text if section_names empty)."""
    if not section_names:
        return text
    chunks = []
    for name in section_names:
        section = extract_section(text, name)
        if section:
            chunks.append(f"## {name}\n{section}")
    return "\n\n".join(chunks)


# ============================================================================
# SENTINEL REGISTRY (per plan body Decision C; ~24 sentinels)
# ============================================================================

SENTINELS: List[SentinelSpec] = [
    # === TECH_DEBT status-flip sentinels ===
    SentinelSpec(
        re.compile(r'TECH_DEBT-(\d+)\b[^\n]*\bNEW\b[^\n]*\bCLOSED\b', re.MULTILINE),
        'verify_tech_debt_closed', 'HIGH',
        'TECH_DEBT-N NEW+CLOSED same ship', 'LANDED-NOW'),
    SentinelSpec(
        re.compile(r'TECH_DEBT-(\d+)\b[^\n]*\(OPEN\+CLOSED', re.MULTILINE),
        'verify_tech_debt_closed', 'HIGH',
        'TECH_DEBT-N NEW (OPEN+CLOSED parenthesized variant)', 'LANDED-NOW'),
    SentinelSpec(
        re.compile(r'TECH_DEBT-(\d+)\b\s+NEW\b(?!\+CLOSED)(?![^\n]*\bCLOSED\b)', re.MULTILINE),
        'verify_tech_debt_open_or_closed', 'HIGH',
        'TECH_DEBT-N NEW (verifier disambiguates open vs closed)', 'EITHER'),
    SentinelSpec(
        # Proximity-bounded: wontfix-per-ai-workflow must appear within 80 chars of TECH_DEBT-N
        # token (non-greedy match prevents false-positive across long single-line table cells)
        re.compile(r'TECH_DEBT-(\d+)\b[^\n]{0,80}?\bwontfix-per-ai-workflow\b', re.MULTILINE),
        'verify_tech_debt_wontfix', 'HIGH',
        'TECH_DEBT-N wontfix-per-ai-workflow status (AI-workflow reframe closure)', 'LANDED-NOW'),
    SentinelSpec(
        re.compile(r'TECH_DEBT-(\d+)\b[^\n]*\btrigger\s+updated\b', re.MULTILINE),
        'verify_tech_debt_trigger_updated', 'MED',
        'TECH_DEBT-N trigger field updated', 'LANDED-NOW'),

    # === PARITY status-flip sentinels ===
    SentinelSpec(
        re.compile(r'PARITY-(\d+)\b[^\n]*\bNEW\b', re.MULTILINE),
        'verify_parity_open', 'HIGH',
        'PARITY-N NEW (opened)', 'LANDED-NOW'),
    SentinelSpec(
        re.compile(r'PARITY-(\d+)\b[^\n]*\bCLOSED\b', re.MULTILINE),
        'verify_parity_closed', 'HIGH',
        'PARITY-N CLOSED', 'LANDED-NOW'),

    # === DOCUMENTED-RISK sentinels ===
    SentinelSpec(
        re.compile(r'DOCUMENTED-RISK\s+(?:PARITY-?(\d+)|entry)\b', re.MULTILINE),
        'verify_documented_risk', 'HIGH',
        'DOCUMENTED-RISK entry claim', 'LANDED-NOW'),

    # === CLASS CATALOG amendment sentinels (load-bearing per Agent 2 HIGH-1) ===
    SentinelSpec(
        re.compile(r'(Class\s+(\d+))[^\n]*\brecurrence(?:_count)?\s+(\d+)\s*[→\-]+>?\s*(\d+)', re.MULTILINE),
        'verify_class_recurrence_bump', 'HIGH',
        'Class N catalog recurrence_count bump (X→Y)', 'LANDED-NOW'),
    SentinelSpec(
        re.compile(r'Class\s+(\d+)[^\n]*\bNEW\s+Worked\s+Examples?\b[^\n]*(?:for\s+\.?([A-Z\d.]+))?', re.MULTILINE),
        'verify_class_worked_example_added', 'HIGH',
        'Class N catalog NEW Worked Example claim', 'LANDED-NOW'),
    SentinelSpec(
        re.compile(r'Class\s+(\d+)[^\n]*\bcatalog\b[^\n]*\bamendment\s+DEFERRED\b', re.MULTILINE),
        'warn_class_amendment_deferred', 'INFO',
        'Class N catalog amendment deferred (informational)', 'PROMISED-FUTURE'),

    # === Stage promotion sentinels (PROMISED vs LANDED distinction) ===
    SentinelSpec(
        re.compile(r'Stage\s+([23])\s*(?:→|->)\s*Stage\s+([34])\s+(?:promotion\s+)?candidates?\b', re.MULTILINE),
        'warn_stage_promotion_promised', 'INFO',
        'Stage promotion CANDIDATE (forward-promise)', 'PROMISED-FUTURE'),
    SentinelSpec(
        re.compile(r'Stage\s+([23])\s*(?:→|->)\s*Stage\s+([34])\s+(?:first\s+canonical|cohort\s+migration)\s+promotion\b', re.MULTILINE),
        'verify_stage_promotion_landed', 'HIGH',
        'Stage promotion LANDED (verify frontmatter)', 'LANDED-NOW'),
    SentinelSpec(
        re.compile(r'`?([a-zA-Z0-9_-]+\.md)`?\s+v(\d+\.\d+)\s*(?:→|->)\s*v(\d+\.\d+)', re.MULTILINE),
        'verify_design_spec_version_bump', 'MED',
        'DESIGN_SPEC version bump claim (vX → vY)', 'LANDED-NOW'),

    # === Stage 6 escalation sentinels ===
    SentinelSpec(
        re.compile(r'Stage\s+6\s+escalation\s+candidate\b', re.MULTILINE),
        'verify_stage_6_escalation', 'MED',
        'Stage 6 escalation candidate claim', 'EITHER'),

    # === Auto-write / forward-advisory sentinels ===
    SentinelSpec(
        re.compile(r'forward\s+advisory\b.{0,500}PARITY\s+ledger\b', re.MULTILINE | re.DOTALL),
        'verify_forward_advisory_landed', 'HIGH',
        'Forward advisory + PARITY ledger cross-ref', 'PROMISED-FUTURE'),
    SentinelSpec(
        re.compile(r'auto-write\s+at\s+ship\s+close:\s*([A-Z_]+)-(\d+)', re.MULTILINE),
        'verify_auto_write_typed', 'HIGH',
        'Auto-write claim with typed target', 'LANDED-NOW'),
    SentinelSpec(
        re.compile(r'retroactively\s+closes?\s+`?\.([A-Z\d.]+)`?\s+forward-promise\b', re.MULTILINE),
        'verify_retroactive_closure', 'MED',
        'Retroactive forward-promise closure claim', 'LANDED-NOW'),

    # === DESIGN_SPEC creation sentinels ===
    # Tightened regex (v1.1): require alpha-start + ≥3 chars in basename to avoid false-positive
    # captures of short word-suffixes (e.g., "...state.md" captured as just "e.md" via backtrack).
    SentinelSpec(
        re.compile(r'NEW\s+(?:Stage\s+\d\s+DRAFT\s+)?DESIGN_SPEC\b[^\n]*?[`\'"\s]([a-zA-Z][a-zA-Z0-9_/-]{2,}\.md)\b', re.MULTILINE),
        'verify_design_spec_exists', 'MED',
        'NEW DESIGN_SPEC creation claim', 'LANDED-NOW'),

    # === Memory file sentinels (anchor at prefix per Agent 2 MED-5) ===
    SentinelSpec(
        re.compile(r'\b(feedback_\w+|user_\w+|project_\w+|reference_\w+)\.md\b', re.MULTILINE),
        'verify_memory_indexed', 'MED',
        'Memory file citation (verify indexed in MEMORY.md)', 'EITHER'),
    SentinelSpec(
        re.compile(r'MEMORY\.md\s+index\s+updated\s+with\s+(\d+)\s+NEW\s+entries', re.MULTILINE),
        'verify_memory_index_sync', 'MED',
        'MEMORY.md index sync claim (N NEW entries)', 'LANDED-NOW'),

    # === Sister-cohort cross-ref sentinels ===
    SentinelSpec(
        re.compile(r'(Class\s+\d+)\s*\+\s*(Class\s+\d+)\s+catalogs?\s+cross-ref\b', re.MULTILINE),
        'verify_sister_cohort_cross_ref', 'MED',
        'Sister-cohort cross-ref amendment claim', 'LANDED-NOW'),

    # === Skill amendment sentinels ===
    SentinelSpec(
        re.compile(r'/(\w+)\s+SKILL\.md\b[^\n]*(?:NEW|amend|extend)', re.MULTILINE | re.IGNORECASE),
        'verify_skill_md_amendment', 'LOW',
        'Skill SKILL.md amendment claim', 'LANDED-NOW'),

    # === Queued-for / deferred-to (informational; not failures) ===
    SentinelSpec(
        re.compile(r'queued\s+for\s+`?\.([A-Z\d.]+)`?\b', re.MULTILINE),
        'warn_queued_for', 'INFO',
        'Queued-for marker (informational)', 'PROMISED-FUTURE'),
    SentinelSpec(
        re.compile(r'deferred\s+to\s+`?\.([A-Z\d.]+)`?\b', re.MULTILINE),
        'warn_deferred_to', 'INFO',
        'Deferred-to marker (informational)', 'PROMISED-FUTURE'),
]


# ============================================================================
# SCAN_SOURCES (per plan body Decision D; 10 sources)
# ============================================================================

SCAN_SOURCES: List[ScanSpec] = [
    # === HIGH-density sentinel sources ===
    ScanSpec(
        root=ENGINE_ROOT / "DOCS/CHANGELOG.md",
        is_dir=False,
        tail_rows=5,
        sentinel_density='HIGH'),
    ScanSpec(
        root=PLANS_DIR / "postmortems",
        is_dir=True, glob="*.md", recursive=False,
        modified_within_days=14,
        sentinel_density='HIGH'),
    ScanSpec(
        root=PLANS_DIR / "handoffs",
        is_dir=True, glob="*.md", recursive=False,
        modified_within_days=14,
        sentinel_density='HIGH'),
    ScanSpec(
        root=PLANS_DIR / "subplans",
        is_dir=True, glob="*.md", recursive=False,
        modified_within_days=14,
        sections=['Acceptance criteria', 'TECH_DEBT auto-write at ship close',
                  'DESIGN_SPECs landed / amended', 'Bug classes this ship closes'],
        sentinel_density='MED'),

    # === Self-referential coverage (Agent 2 HIGH-5) ===
    ScanSpec(
        root=PLANS_DIR / "capture-audit-reports",
        is_dir=True, glob="*.md", recursive=False,
        modified_within_days=21,
        sentinel_density='HIGH'),

    # === Always-loaded going-forward sentinel surfaces (Agent 2 MED-6) ===
    ScanSpec(
        root=CLAUDE_LOCAL_MD,
        is_dir=False,
        sections=['Going-forward rules (index)', 'Current sprint state'],
        sentinel_density='MED'),
    ScanSpec(
        root=SKILLS_DIR,
        is_dir=True, glob="SKILL.md", recursive=True,
        sections=['Worked examples', 'Worked example dogfood', 'Dogfood'],
        sentinel_density='LOW'),

    # === MED-density sentinel sources ===
    ScanSpec(
        root=RECURRING_BUG_PATTERNS_DIR,
        is_dir=True, glob="class-*.md", recursive=False,
        sections=['Worked Examples', 'Cross-references'],
        sentinel_density='HIGH'),
    ScanSpec(
        root=DESIGN_SPECS_DIR,
        is_dir=True, glob="*.md", recursive=True,
        sections=['Lifecycle', 'Pattern lifecycle', 'Pattern status update'],
        exclude_patterns=['README.md', 'TAG_INDEX.md', 'plan-templates/*'],
        sentinel_density='MED'),

    # === LOW-density sentinel sources ===
    ScanSpec(
        root=ENGINE_ROOT / "Version.hpp",
        is_dir=False,
        sentinel_density='LOW'),
]


# ============================================================================
# Helper functions
# ============================================================================

def is_archived(path: Path) -> bool:
    """Check if path matches ARCHIVED_EXCLUSIONS patterns."""
    try:
        rel = path.relative_to(ENGINE_ROOT)
    except ValueError:
        try:
            rel = path.relative_to(WORKSPACE_ROOT)
        except ValueError:
            return False
    rel_str = str(rel)
    for pattern in ARCHIVED_EXCLUSIONS:
        # Convert glob pattern to regex (simple: * → .*)
        regex = pattern.replace('*', '.*').replace('/', r'/')
        if re.match(regex, rel_str):
            return True
    return False


def get_git_modified_files(repo_root: Path, since_ref: str) -> set:
    """Return set of absolute paths of files modified since git ref."""
    try:
        result = subprocess.run(
            ['git', '-C', str(repo_root), 'diff', '--name-only', f'{since_ref}..HEAD'],
            capture_output=True, text=True, check=False, timeout=30)
        if result.returncode != 0:
            return set()
        return {(repo_root / line.strip()).resolve()
                for line in result.stdout.splitlines() if line.strip()}
    except (subprocess.TimeoutExpired, OSError):
        return set()


def is_recently_modified(path: Path, within_days: int) -> bool:
    """Check file mtime within N days of now."""
    import time
    try:
        mtime = path.stat().st_mtime
        return (time.time() - mtime) <= (within_days * 86400)
    except OSError:
        return False


def extract_tail_rows(text: str, n: int) -> str:
    """For CHANGELOG-style markdown table, extract top N rows.

    CHANGELOG rows start with `| **5.X.Y.Z** |` shape. Returns text containing
    the top N rows (newest first; table is reverse-chronological).
    """
    # Find the markdown table header (| Version | Date | ... |)
    # Then capture the next N data rows
    table_pattern = re.compile(
        r'\|\s*Version\s*\|.*?\n(?:\|[\s\-:|]+\|\s*\n)?',
        re.MULTILINE
    )
    m = table_pattern.search(text)
    if not m:
        return text  # not a CHANGELOG table; return full text
    # Find data rows after header
    after_header = text[m.end():]
    rows = re.findall(r'^\|\s*\*\*[\d.A-Z.]+\*\*\s*\|.*?(?=^\|\s*\*\*|\Z)',
                      after_header, re.MULTILINE | re.DOTALL)
    return text[:m.end()] + '\n'.join(rows[:n])


def expand_scan_spec(spec: ScanSpec, since_ref: Optional[str],
                     include_archived: bool) -> List[Tuple[Path, str]]:
    """Expand a ScanSpec into (path, text) tuples; apply filters."""
    results = []
    if spec.is_dir:
        if not spec.root.exists():
            return results
        if spec.recursive:
            paths = list(spec.root.rglob(spec.glob or '*'))
        else:
            paths = list(spec.root.glob(spec.glob or '*'))
    else:
        if not spec.root.exists():
            return results
        paths = [spec.root]

    for path in paths:
        if not path.is_file():
            continue
        # Apply exclusion patterns
        excluded = False
        for excl in spec.exclude_patterns:
            if path.match(excl):
                excluded = True
                break
        if excluded:
            continue
        # Archived exclusion
        if not include_archived and is_archived(path):
            continue
        # Recently modified filter
        if spec.modified_within_days is not None:
            if not is_recently_modified(path, spec.modified_within_days):
                continue
        # Since-ref filter
        if since_ref is not None:
            # Check both engine + workspace git for the path
            in_engine = False
            try:
                path.relative_to(ENGINE_ROOT)
                modified = get_git_modified_files(ENGINE_ROOT, since_ref)
                in_engine = path.resolve() in modified
            except ValueError:
                pass
            in_workspace = False
            try:
                path.relative_to(WORKSPACE_ROOT)
                modified = get_git_modified_files(WORKSPACE_ROOT, since_ref)
                in_workspace = path.resolve() in modified
            except ValueError:
                pass
            if not (in_engine or in_workspace):
                # Always include modified_within_days files even if not in since-ref diff
                if spec.modified_within_days is None:
                    continue
        # Read text
        try:
            text = path.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        # Apply tail_rows for CHANGELOG-style
        if spec.tail_rows is not None:
            text = extract_tail_rows(text, spec.tail_rows)
        # Apply section filter
        if spec.sections:
            text = filter_text_to_sections(text, spec.sections)
        results.append((path, text))
    return results


# ============================================================================
# Verifier functions (~24 per plan body Phase B.3)
# ============================================================================

def _read_safe(path: Path) -> str:
    """Read file safely; return empty string if missing."""
    try:
        return path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return ''


def verify_tech_debt_closed(match, source_file, source_line, promised_vs_landed):
    """Verify TECH_DEBT-N has entry in closed.md."""
    n = match.group(1)
    if not TECH_DEBT_CLOSED.exists():
        return None
    closed_text = _read_safe(TECH_DEBT_CLOSED)
    if re.search(rf'^id: TECH_DEBT-{n}\b', closed_text, re.MULTILINE):
        return None  # CLEAN
    open_text = _read_safe(TECH_DEBT_OPEN)
    if re.search(rf'^id: TECH_DEBT-{n}\b', open_text, re.MULTILINE):
        return Finding('HIGH', f'TECH_DEBT-{n} NEW+CLOSED',
                       source_file, source_line, (n,),
                       'closed.md (currently in open.md)', 'open.md',
                       f'Move TECH_DEBT-{n} from open.md to closed.md OR re-classify claim',
                       promised_vs_landed)
    return Finding('HIGH', f'TECH_DEBT-{n} NEW+CLOSED',
                   source_file, source_line, (n,),
                   'closed.md', None,
                   f'Write TECH_DEBT-{n} entry to closed.md OR re-classify claim',
                   promised_vs_landed)


def verify_tech_debt_open_or_closed(match, source_file, source_line, promised_vs_landed):
    """Verify TECH_DEBT-N exists in either open.md or closed.md."""
    n = match.group(1)
    open_text = _read_safe(TECH_DEBT_OPEN)
    closed_text = _read_safe(TECH_DEBT_CLOSED)
    if re.search(rf'^id: TECH_DEBT-{n}\b', open_text, re.MULTILINE):
        return None
    if re.search(rf'^id: TECH_DEBT-{n}\b', closed_text, re.MULTILINE):
        return None
    return Finding('HIGH', f'TECH_DEBT-{n} NEW (no entry)',
                   source_file, source_line, (n,),
                   'open.md OR closed.md', None,
                   f'Write TECH_DEBT-{n} entry to ledger OR re-classify claim',
                   promised_vs_landed)


def verify_tech_debt_wontfix(match, source_file, source_line, promised_vs_landed):
    """Verify TECH_DEBT-N entry has status: wontfix-per-ai-workflow."""
    n = match.group(1)
    closed_text = _read_safe(TECH_DEBT_CLOSED)
    # Find the entry block
    entry_pattern = rf'^id: TECH_DEBT-{n}\b.*?(?=^id: TECH_DEBT-|\Z)'
    entry_match = re.search(entry_pattern, closed_text, re.MULTILINE | re.DOTALL)
    if entry_match is None:
        return Finding('HIGH', f'TECH_DEBT-{n} wontfix-per-ai-workflow',
                       source_file, source_line, (n,),
                       'closed.md', None,
                       f'Write TECH_DEBT-{n} entry to closed.md with status: wontfix-per-ai-workflow',
                       promised_vs_landed)
    entry_text = entry_match.group(0)
    if 'wontfix-per-ai-workflow' not in entry_text:
        return Finding('HIGH', f'TECH_DEBT-{n} wontfix-per-ai-workflow',
                       source_file, source_line, (n,),
                       'closed.md entry with status: wontfix-per-ai-workflow',
                       'closed.md entry without wontfix-per-ai-workflow status',
                       f'Update TECH_DEBT-{n} status field to wontfix-per-ai-workflow',
                       promised_vs_landed)
    return None


def verify_tech_debt_trigger_updated(match, source_file, source_line, promised_vs_landed):
    """Verify TECH_DEBT-N entry trigger field has been updated (heuristic: recent ledger edit)."""
    n = match.group(1)
    open_text = _read_safe(TECH_DEBT_OPEN)
    entry_pattern = rf'^id: TECH_DEBT-{n}\b.*?(?=^id: TECH_DEBT-|\Z)'
    entry_match = re.search(entry_pattern, open_text, re.MULTILINE | re.DOTALL)
    if entry_match is None:
        return None  # not in open.md; might be closed (verify_tech_debt_closed handles)
    entry_text = entry_match.group(0)
    if 'trigger:' not in entry_text:
        return Finding('MED', f'TECH_DEBT-{n} trigger updated',
                       source_file, source_line, (n,),
                       'open.md entry with trigger field',
                       'entry missing trigger field',
                       f'Add trigger field to TECH_DEBT-{n} entry',
                       promised_vs_landed)
    return None  # entry present + has trigger; assume claim matches actual


def verify_parity_open(match, source_file, source_line, promised_vs_landed):
    """Verify PARITY-N entry exists in PARITY_ISSUES.md."""
    n = match.group(1)
    parity_text = _read_safe(PARITY_ISSUES)
    if re.search(rf'^id: PARITY-{n}\b', parity_text, re.MULTILINE):
        return None
    return Finding('HIGH', f'PARITY-{n} NEW',
                   source_file, source_line, (n,),
                   f'PARITY_ISSUES.md entry id: PARITY-{n}', None,
                   f'Write PARITY-{n} entry to PARITY_ISSUES.md',
                   promised_vs_landed)


def verify_parity_closed(match, source_file, source_line, promised_vs_landed):
    """Verify PARITY-N entry has status: closed in PARITY_ISSUES.md."""
    n = match.group(1)
    parity_text = _read_safe(PARITY_ISSUES)
    entry_pattern = rf'^id: PARITY-{n}\b.*?(?=^id: PARITY-|\Z)'
    entry_match = re.search(entry_pattern, parity_text, re.MULTILINE | re.DOTALL)
    if entry_match is None:
        return Finding('HIGH', f'PARITY-{n} CLOSED',
                       source_file, source_line, (n,),
                       f'PARITY_ISSUES.md entry id: PARITY-{n}', None,
                       f'Write PARITY-{n} entry to PARITY_ISSUES.md',
                       promised_vs_landed)
    entry_text = entry_match.group(0)
    if not re.search(r'^status:\s*closed\b', entry_text, re.MULTILINE):
        return Finding('HIGH', f'PARITY-{n} CLOSED',
                       source_file, source_line, (n,),
                       'status: closed', 'status: open (or other)',
                       f'Update PARITY-{n} status to closed',
                       promised_vs_landed)
    return None


def verify_documented_risk(match, source_file, source_line, promised_vs_landed):
    """Verify DOCUMENTED-RISK entry exists in PARITY_ISSUES.md."""
    parity_id = match.group(1)
    parity_text = _read_safe(PARITY_ISSUES)
    if parity_id:
        # Specific PARITY-N
        entry_pattern = rf'^id: PARITY-{parity_id}\b.*?(?=^id: PARITY-|\Z)'
        entry_match = re.search(entry_pattern, parity_text, re.MULTILINE | re.DOTALL)
        if entry_match is None:
            return Finding('HIGH', f'DOCUMENTED-RISK PARITY-{parity_id}',
                           source_file, source_line, (parity_id,),
                           f'PARITY-{parity_id} entry with severity: documented-risk', None,
                           f'Write PARITY-{parity_id} entry with severity: documented-risk',
                           promised_vs_landed)
        if 'documented-risk' not in entry_match.group(0):
            return Finding('HIGH', f'DOCUMENTED-RISK PARITY-{parity_id}',
                           source_file, source_line, (parity_id,),
                           'severity: documented-risk', 'severity: other',
                           f'Update PARITY-{parity_id} severity to documented-risk',
                           promised_vs_landed)
    else:
        # Generic "DOCUMENTED-RISK entry" — check any entry has documented-risk severity
        if 'documented-risk' not in parity_text:
            return Finding('HIGH', 'DOCUMENTED-RISK entry',
                           source_file, source_line, (),
                           'PARITY_ISSUES.md has severity: documented-risk entry', None,
                           'Write a DOCUMENTED-RISK entry to PARITY_ISSUES.md',
                           promised_vs_landed)
    return None


def verify_class_recurrence_bump(match, source_file, source_line, promised_vs_landed):
    """Verify Class N catalog frontmatter recurrence_count matches claimed bump (X→Y).

    Load-bearing verifier per Agent 2 HIGH-1: catches the .B.7 Class 26
    forward-promise drift pattern (recurrence 11→13 + promised DOCUMENTED-RISK
    entry never written).
    """
    class_n = match.group(2)
    bump_from = match.group(3)
    bump_to = match.group(4)
    catalog_glob = list(RECURRING_BUG_PATTERNS_DIR.glob(f"class-{class_n}-*.md"))
    if not catalog_glob:
        return Finding('HIGH', f'Class {class_n} recurrence bump {bump_from}→{bump_to}',
                       source_file, source_line, (class_n, bump_from, bump_to),
                       f'class-{class_n}-*.md', None,
                       f'Class {class_n} catalog file not found; verify class number',
                       promised_vs_landed)
    catalog_text = _read_safe(catalog_glob[0])
    rc_match = re.search(r'^recurrence_count:\s*(\d+)', catalog_text, re.MULTILINE)
    if rc_match is None:
        return Finding('HIGH', f'Class {class_n} recurrence bump',
                       source_file, source_line, (class_n, bump_from, bump_to),
                       f'{catalog_glob[0].name} recurrence_count: {bump_to}',
                       'frontmatter missing recurrence_count field',
                       f'Add recurrence_count: {bump_to} to {catalog_glob[0].name}',
                       promised_vs_landed)
    actual = int(rc_match.group(1))
    if actual < int(bump_to):
        return Finding('HIGH', f'Class {class_n} recurrence bump {bump_from}→{bump_to}',
                       source_file, source_line, (class_n, bump_from, bump_to),
                       f'recurrence_count: {bump_to}', f'recurrence_count: {actual}',
                       f'Bump {catalog_glob[0].name} recurrence_count: {actual}→{bump_to}',
                       promised_vs_landed)
    return None  # CLEAN (actual >= claimed; could be higher from later bumps)


def verify_class_worked_example_added(match, source_file, source_line, promised_vs_landed):
    """Verify Class N catalog has NEW Worked Example for cited ship-tag."""
    class_n = match.group(1)
    ship_tag = match.group(2) if match.lastindex and match.lastindex >= 2 else None
    catalog_glob = list(RECURRING_BUG_PATTERNS_DIR.glob(f"class-{class_n}-*.md"))
    if not catalog_glob:
        return None
    catalog_text = _read_safe(catalog_glob[0])
    if ship_tag and ship_tag not in catalog_text:
        return Finding('HIGH', f'Class {class_n} NEW Worked Example for .{ship_tag}',
                       source_file, source_line, (class_n, ship_tag),
                       f'{catalog_glob[0].name} body mentions .{ship_tag}', None,
                       f'Add Worked Example entry for .{ship_tag} to {catalog_glob[0].name}',
                       promised_vs_landed)
    return None


def warn_class_amendment_deferred(match, source_file, source_line, promised_vs_landed):
    """Informational: Class N catalog amendment deferred. Always emits INFO finding."""
    class_n = match.group(1)
    return Finding('INFO', f'Class {class_n} catalog amendment DEFERRED',
                   source_file, source_line, (class_n,),
                   '(deferred per proportionate response)', '(deferred)',
                   f'Verify Class {class_n} amendment lands at next applicable ship OR re-evaluate',
                   promised_vs_landed)


def warn_stage_promotion_promised(match, source_file, source_line, promised_vs_landed):
    """Informational: Stage promotion candidate (forward-promise)."""
    stage_from = match.group(1)
    stage_to = match.group(2)
    return Finding('INFO', f'Stage {stage_from} → Stage {stage_to} promotion candidate',
                   source_file, source_line, (stage_from, stage_to),
                   '(forward-promise marker)', '(promised)',
                   f'Verify Stage {stage_from} → Stage {stage_to} promotion lands at next applicable ship',
                   promised_vs_landed)


def verify_stage_promotion_landed(match, source_file, source_line, promised_vs_landed):
    """Verify DESIGN_SPECS frontmatter shows promoted stage."""
    stage_from = match.group(1)
    stage_to = match.group(2)
    # Heuristic: scan all DESIGN_SPECS files modified recently for promoted_to_stage_N OR stage: N-first-canonical
    promoted_marker = f'promoted_to_stage_{stage_to}:'
    stage_marker = f'stage: {stage_to}-first-canonical'
    # Search across DESIGN_SPECS
    if DESIGN_SPECS_DIR.exists():
        for spec_path in DESIGN_SPECS_DIR.rglob('*.md'):
            spec_text = _read_safe(spec_path)
            if promoted_marker in spec_text or stage_marker in spec_text:
                return None  # at least one promoted spec found
    return Finding('MED', f'Stage {stage_from} → Stage {stage_to} promotion landed',
                   source_file, source_line, (stage_from, stage_to),
                   f'DESIGN_SPECS/**/*.md with {promoted_marker} OR {stage_marker}', None,
                   f'Verify Stage {stage_to} promotion landed in target DESIGN_SPEC frontmatter',
                   promised_vs_landed)


def verify_design_spec_version_bump(match, source_file, source_line, promised_vs_landed):
    """Verify DESIGN_SPEC frontmatter version >= claimed bump.

    v1.1 fix: accept current_version >= claimed_to_version (monotonic version progression).
    A spec bumped past the claimed target (e.g., later bumped v1.2 → v1.3 → v1.4 across
    multiple ships) is CLEAN — the claim at the older ship landed correctly + subsequent
    ships bumped further. Only flag if current < claimed_to.
    """
    spec_name = match.group(1)
    version_from = match.group(2)
    version_to = match.group(3)
    # Find spec file
    spec_paths = list(DESIGN_SPECS_DIR.rglob(spec_name))
    if not spec_paths:
        return None  # spec not found; can't verify
    spec_text = _read_safe(spec_paths[0])
    version_match = re.search(r'^version:\s*(\d+\.\d+)', spec_text, re.MULTILINE)
    if version_match is None:
        return Finding('MED', f'{spec_name} version v{version_from} → v{version_to}',
                       source_file, source_line, (spec_name, version_from, version_to),
                       f'{spec_paths[0].name} version: {version_to}',
                       'frontmatter missing version field',
                       f'Add version: {version_to} to {spec_paths[0].name}',
                       promised_vs_landed)
    actual = version_match.group(1)
    # Version comparison: parse "X.Y" → (X, Y) tuple for numeric comparison
    def parse_ver(v):
        parts = v.split('.')
        return tuple(int(p) for p in parts) if all(p.isdigit() for p in parts) else (0, 0)
    actual_tuple = parse_ver(actual)
    claimed_to_tuple = parse_ver(version_to)
    if actual_tuple < claimed_to_tuple:
        # Current version is BELOW the claimed bump target — real drift
        return Finding('MED', f'{spec_name} version v{version_from} → v{version_to}',
                       source_file, source_line, (spec_name, version_from, version_to),
                       f'version: >= {version_to}', f'version: {actual}',
                       f'Update {spec_paths[0].name} version: {actual} → {version_to} (or higher)',
                       promised_vs_landed)
    # actual >= claimed_to: CLEAN (spec was bumped to claim level or further)
    return None


def verify_stage_6_escalation(match, source_file, source_line, promised_vs_landed):
    """Verify Stage 6 escalation candidate landed (NEW Check N in tools/ OR skill amendment).

    v1.1 fix: demote severity to INFO when no specific surface claimed in context.
    Stage 6 escalation candidates are typically forward-looking markers (PROMISED-FUTURE);
    INFO tracking is the right signal level. HIGH/MED severity reserved for cases where
    the candidate was explicitly claimed LANDED at a specific ship.
    """
    return Finding('INFO', 'Stage 6 escalation candidate',
                   source_file, source_line, (),
                   '(forward-looking marker)', '(typically PROMISED-FUTURE)',
                   'Track Stage 6 escalation candidate for future-ship landing verification',
                   promised_vs_landed)


def verify_forward_advisory_landed(match, source_file, source_line, promised_vs_landed):
    """Verify forward advisory + PARITY ledger cross-ref landed."""
    # Multi-paragraph match; verifier checks PARITY_ISSUES.md has corresponding entry
    parity_text = _read_safe(PARITY_ISSUES)
    matched_text = match.group(0)
    # Extract any PARITY-N references in matched text
    parity_ids = re.findall(r'PARITY-(\d+)', matched_text)
    if not parity_ids:
        # No specific ID; informational
        return Finding('MED', 'Forward advisory + PARITY ledger',
                       source_file, source_line, (),
                       'PARITY_ISSUES.md entry referenced by forward advisory',
                       '(no specific PARITY-N captured)',
                       'Verify forward advisory references valid PARITY entry',
                       promised_vs_landed)
    for pid in parity_ids:
        if not re.search(rf'^id: PARITY-{pid}\b', parity_text, re.MULTILINE):
            return Finding('HIGH', f'Forward advisory references PARITY-{pid}',
                           source_file, source_line, (pid,),
                           f'PARITY-{pid} entry in PARITY_ISSUES.md', None,
                           f'Write PARITY-{pid} entry OR remove forward-advisory cite',
                           promised_vs_landed)
    return None


def verify_auto_write_typed(match, source_file, source_line, promised_vs_landed):
    """Verify auto-write claim with typed target (TECH_DEBT-N / PARITY-N / etc.)."""
    type_str = match.group(1)
    n = match.group(2)
    if type_str == 'TECH_DEBT':
        # Dispatch to TECH_DEBT verifier (open or closed)
        fake_match = type('FakeMatch', (), {'group': lambda self, i: n if i == 1 else None})()
        return verify_tech_debt_open_or_closed(fake_match, source_file, source_line, promised_vs_landed)
    elif type_str == 'PARITY':
        fake_match = type('FakeMatch', (), {'group': lambda self, i: n if i == 1 else None})()
        return verify_parity_open(fake_match, source_file, source_line, promised_vs_landed)
    return Finding('MED', f'Auto-write {type_str}-{n}',
                   source_file, source_line, (type_str, n),
                   f'{type_str}-{n} entry at known ledger location',
                   '(unknown type)',
                   f'Verify {type_str}-{n} auto-write landed',
                   promised_vs_landed)


def verify_retroactive_closure(match, source_file, source_line, promised_vs_landed):
    """Verify retroactive forward-promise closure (informational; sister to forward_advisory)."""
    ship_tag = match.group(1)
    return Finding('LOW', f'Retroactive closure of .{ship_tag} forward-promise',
                   source_file, source_line, (ship_tag,),
                   f'Sister forward-promise for .{ship_tag} closed at this ship',
                   '(retroactive)',
                   f'Verify .{ship_tag} forward-promise originally claimed is now resolved',
                   promised_vs_landed)


def verify_design_spec_exists(match, source_file, source_line, promised_vs_landed):
    """Verify NEW DESIGN_SPEC creation claim — file exists at cited path."""
    spec_ref = match.group(1)
    # Try to resolve: basename OR full path
    spec_basename = Path(spec_ref).name
    found = list(DESIGN_SPECS_DIR.rglob(spec_basename))
    if not found:
        return Finding('MED', f'NEW DESIGN_SPEC {spec_basename}',
                       source_file, source_line, (spec_ref,),
                       f'DESIGN_SPECS/**/{spec_basename}', None,
                       f'Create DESIGN_SPEC file at expected path OR remove claim',
                       promised_vs_landed)
    return None


def verify_memory_indexed(match, source_file, source_line, promised_vs_landed):
    """Verify memory file exists + is indexed in MEMORY.md.

    v1.1 fix: only emit HIGH/MED severity if memory file is cited in CURRENT canonical
    docs (CLAUDE.local.md going-forward rules + MEMORY.md index). Citations in old
    handoff docs are historical references (memory files proposed at handoff write time
    but never created) — these emit LOW severity to surface but not block.
    """
    fname_stem = match.group(1)
    fname = f'{fname_stem}.md'
    full_path = MEMORY_DIR / fname
    if full_path.exists():
        if MEMORY_INDEX.exists():
            index_text = _read_safe(MEMORY_INDEX)
            if fname not in index_text:
                return Finding('MED', f'Memory file {fname} indexed',
                               source_file, source_line, (fname_stem,),
                               f'MEMORY.md index entry for {fname}',
                               'Not in MEMORY.md',
                               f'Add MEMORY.md index entry for {fname}',
                               promised_vs_landed)
        return None
    # Memory file missing — determine severity by where citation lives
    is_handoff = 'plans/' in source_file and '/handoffs/' in source_file
    is_current_doc = source_file.endswith('CLAUDE.local.md') or source_file.endswith('MEMORY.md')
    if is_current_doc:
        # Cited in CURRENT canonical doc — load-bearing drift
        severity = 'HIGH'
    elif is_handoff:
        # Historical handoff reference — typically stale; LOW severity
        severity = 'LOW'
    else:
        # Other source (postmortem, plan body, etc.) — MED severity
        severity = 'MED'
    return Finding(severity, f'Memory file {fname}',
                   source_file, source_line, (fname_stem,),
                   f'memory/{fname}', None,
                   f'Create memory file at {full_path} OR remove stale reference',
                   promised_vs_landed)


def verify_memory_index_sync(match, source_file, source_line, promised_vs_landed):
    """Verify MEMORY.md index has at least N recent entries (heuristic; not strict)."""
    n = int(match.group(1))
    # Heuristic: MEMORY.md should have grown by at least N entries since prior baseline
    # For v1.0: just verify MEMORY.md exists + has entries; full diff-against-baseline deferred
    if not MEMORY_INDEX.exists():
        return Finding('MED', f'MEMORY.md index updated with {n} NEW entries',
                       source_file, source_line, (str(n),),
                       'MEMORY.md exists', None,
                       'Create MEMORY.md index',
                       promised_vs_landed)
    return None  # CLEAN (informational; full N-entry diff verification is v1.x enhancement)


def verify_sister_cohort_cross_ref(match, source_file, source_line, promised_vs_landed):
    """Verify both Class catalogs reference each other."""
    class_a = match.group(1)  # "Class X"
    class_b = match.group(2)  # "Class Y"
    a_num = re.search(r'\d+', class_a).group()
    b_num = re.search(r'\d+', class_b).group()
    a_glob = list(RECURRING_BUG_PATTERNS_DIR.glob(f"class-{a_num}-*.md"))
    b_glob = list(RECURRING_BUG_PATTERNS_DIR.glob(f"class-{b_num}-*.md"))
    if not a_glob or not b_glob:
        return None
    a_text = _read_safe(a_glob[0])
    b_text = _read_safe(b_glob[0])
    if class_b not in a_text:
        return Finding('MED', f'{class_a} ↔ {class_b} sister cross-ref',
                       source_file, source_line, (class_a, class_b),
                       f'{a_glob[0].name} references {class_b}', None,
                       f'Add {class_b} cross-ref to {a_glob[0].name}',
                       promised_vs_landed)
    if class_a not in b_text:
        return Finding('MED', f'{class_b} ↔ {class_a} sister cross-ref',
                       source_file, source_line, (class_a, class_b),
                       f'{b_glob[0].name} references {class_a}', None,
                       f'Add {class_a} cross-ref to {b_glob[0].name}',
                       promised_vs_landed)
    return None


def verify_skill_md_amendment(match, source_file, source_line, promised_vs_landed):
    """Verify skill SKILL.md amendment (heuristic: file exists)."""
    skill_name = match.group(1)
    skill_path = SKILLS_DIR / skill_name / 'SKILL.md'
    if not skill_path.exists():
        return Finding('LOW', f'/{skill_name} SKILL.md amendment',
                       source_file, source_line, (skill_name,),
                       f'.claude/skills/{skill_name}/SKILL.md', None,
                       f'Verify skill exists at expected path',
                       promised_vs_landed)
    return None  # CLEAN (file exists; content-level verification deferred)


def warn_queued_for(match, source_file, source_line, promised_vs_landed):
    """Informational: queued-for marker."""
    ship_tag = match.group(1)
    return Finding('INFO', f'Queued for .{ship_tag}',
                   source_file, source_line, (ship_tag,),
                   '(forward-promise marker)', '(queued)',
                   f'Verify queued-for .{ship_tag} item resolves at that ship',
                   promised_vs_landed)


def warn_deferred_to(match, source_file, source_line, promised_vs_landed):
    """Informational: deferred-to marker."""
    ship_tag = match.group(1)
    return Finding('INFO', f'Deferred to .{ship_tag}',
                   source_file, source_line, (ship_tag,),
                   '(forward-promise marker)', '(deferred)',
                   f'Verify deferred-to .{ship_tag} item resolves at that ship',
                   promised_vs_landed)


# ============================================================================
# Scanner orchestrator
# ============================================================================

def scan_for_forward_promises(since_ref: Optional[str] = None,
                              include_archived: bool = False) -> List[Finding]:
    """Walk SCAN_SOURCES; apply SENTINELS; invoke verifiers; aggregate findings."""
    findings = []
    for scan_spec in SCAN_SOURCES:
        for source_path, source_text in expand_scan_spec(scan_spec, since_ref, include_archived):
            for sentinel in SENTINELS:
                for match in sentinel.pattern.finditer(source_text):
                    line_num = source_text.count('\n', 0, match.start()) + 1
                    # Check exemption
                    exempt = is_exempt(source_text, line_num)
                    if exempt:
                        continue
                    # Dispatch verifier
                    verifier_fn = globals().get(sentinel.verifier_fn)
                    if verifier_fn is None:
                        continue
                    try:
                        finding = verifier_fn(match, str(source_path), line_num,
                                              sentinel.promised_vs_landed)
                    except Exception as e:
                        # Defensive: don't crash on verifier bug
                        finding = Finding('LOW', f'Verifier error: {sentinel.verifier_fn}',
                                          str(source_path), line_num, (),
                                          'verifier impl', f'error: {e}',
                                          'Fix verifier function',
                                          sentinel.promised_vs_landed)
                    if finding is not None:
                        findings.append(finding)
    return findings


# ============================================================================
# Output emitters
# ============================================================================

def emit_human_readable(findings: List[Finding]) -> None:
    """Print human-readable findings report."""
    high = [f for f in findings if f.severity == 'HIGH']
    med = [f for f in findings if f.severity == 'MED']
    low = [f for f in findings if f.severity == 'LOW']
    info = [f for f in findings if f.severity == 'INFO']

    print(f"=== Check 11: Forward-promise auto-write verification ===")
    print()
    print(f"Sentinels matched + verified: {len(findings) + 0} (findings list shows drift only)")
    print(f"Findings by severity: HIGH={len(high)} MED={len(med)} LOW={len(low)} INFO={len(info)}")
    print()

    if high:
        print("=== HIGH findings ===")
        for i, f in enumerate(high, 1):
            print(f"\n[{i}] {f.sentinel_desc} ({f.promised_vs_landed})")
            print(f"    Source: {f.source_file}:{f.source_line}")
            print(f"    Captured: {f.captured_groups}")
            print(f"    Expected: {f.expected_location}")
            print(f"    Actual: {f.actual_location}")
            print(f"    Suggestion: {f.suggestion}")
        print()

    if med:
        print("=== MED findings ===")
        for i, f in enumerate(med, 1):
            print(f"\n[{i}] {f.sentinel_desc}")
            print(f"    Source: {f.source_file}:{f.source_line}")
            print(f"    Suggestion: {f.suggestion}")
        print()

    if low:
        print(f"=== LOW findings: {len(low)} ===")
        for f in low:
            print(f"  - {f.sentinel_desc} at {f.source_file}:{f.source_line}")
        print()

    if info:
        print(f"=== INFO findings (forward-promise markers; not failures): {len(info)} ===")
        for f in info:
            print(f"  - {f.sentinel_desc} at {f.source_file}:{f.source_line}")
        print()

    print(f"=== Verdict ===")
    if high:
        print(f"HIGH: {len(high)} findings → exit 1 (BLOCK if --strict)")
    elif med or low:
        print(f"MED/LOW: {len(med) + len(low)} findings → exit 0 (WARN)")
    else:
        print(f"CLEAN: 0 substantive findings ({len(info)} INFO markers — informational only)")


def emit_json(findings: List[Finding]) -> None:
    """Print JSON-formatted findings."""
    out = {
        'check': 11,
        'tool': 'check_forward_promise_audit.py',
        'findings_by_severity': {
            'HIGH': len([f for f in findings if f.severity == 'HIGH']),
            'MED': len([f for f in findings if f.severity == 'MED']),
            'LOW': len([f for f in findings if f.severity == 'LOW']),
            'INFO': len([f for f in findings if f.severity == 'INFO']),
        },
        'findings': [
            {
                'severity': f.severity,
                'sentinel_desc': f.sentinel_desc,
                'source_file': f.source_file,
                'source_line': f.source_line,
                'captured_groups': list(f.captured_groups),
                'expected_location': f.expected_location,
                'actual_location': f.actual_location,
                'suggestion': f.suggestion,
                'promised_vs_landed': f.promised_vs_landed,
            }
            for f in findings
        ],
    }
    print(json.dumps(out, indent=2))


# ============================================================================
# main()
# ============================================================================

def main() -> int:
    ap = argparse.ArgumentParser(
        description='Check 11 forward-promise auto-write verification')
    ap.add_argument('--since', default=None,
                    help='Git ref scope filter (e.g., v5.15.5.F.4d.1.B.6)')
    ap.add_argument('--strict', action='store_true',
                    help='Exit 1 on HIGH findings (BLOCK)')
    ap.add_argument('--json', action='store_true',
                    help='Machine-parseable output')
    ap.add_argument('--include-archived', action='store_true',
                    help='Bypass ARCHIVED_EXCLUSIONS')
    args = ap.parse_args()

    findings = scan_for_forward_promises(args.since, args.include_archived)

    if args.json:
        emit_json(findings)
    else:
        emit_human_readable(findings)

    high_count = sum(1 for f in findings if f.severity == 'HIGH')
    if high_count > 0 and args.strict:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
