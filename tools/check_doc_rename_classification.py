#!/usr/bin/env python3
"""
tools/check_doc_rename_classification.py — Pre-classification tool for .D.1 doc-sweep ship.

Detects per-core → per-node terminology drift candidates in .md files across DOCS/ + DESIGN_SPECS/
+ plans/ scope. Classifies each hit per 12-class matrix; emits TSV for operator triage.

Sister to:
  tools/check_plan_body_symbol_existence.py (B-Plus pattern)
  tools/check_forward_promise_audit.py (Check 11 .D ship pattern)

M7 8th canonical structural enforcement application per
DESIGN_SPECS/meta-disciplines/structural-enforcement-when-memory-insufficient.md.

Per .D.1 plan body Phase A.1 + cycle 1+2+3 audit amendments.

=== CLASSIFICATION → ACTION MAPPING (read before triaging TSV) ===

The tool is a FIRST-PASS classifier; the TSV is operator-reviewed at Phase A.4 +
per-phase. It biases toward LEAVE on the keep/historical/transition classes and
RENAME on narrative. Confusion-prevention: the narrative-* classes ALL map to the
SAME action (RENAME). The aspirational-vs-current-state distinction is rationale-
tagging for the audit trail only — there is NO action difference between them. Do
not look for a different edit behavior between narrative-aspirational and
narrative-current-state; both mean "rename per-core -> per-node here."

  LEAVE                       : keep-token-context, archived-file,
                                current-changelog-row, transition-documentation,
                                code-fence-cite, ship-tag-citation, historical-tense
  RENAME                      : narrative-aspirational, narrative-current-state
  RENAME-WITH-CONFIRM         : code-fence-target
  SISTER-RENAME-CANDIDATE     : memory-link-crossref
  SISTER-COHORT-XREF          : claude-md-section-crossref

=== KNOWN LIMITATIONS (manual-verify surfaces; per .D.1 R3 over-rename risk) ===

1. SECTION-HEADER CONTEXT NOT TRACKED. Classification is per-LINE. A line inside a
   historical/worked-example section ("## Historical context (v5.10)") that lacks a
   per-line past-tense verb is classified narrative-current-state -> RENAME even
   though the whole section is historical and should be LEFT. MITIGATION: at TSV
   triage, manually verify RENAME hits that fall under headers containing
   "Historical" / "Worked example" / "Changelog" / a vN.M ship tag. The comprehensive
   `rg` pass at plan body Phase H.2 + operator review at Phase A.4 are the safety nets.
   This is a DELIBERATE non-feature: a section-context state machine was rejected as
   gold-plating (feedback_framework_layer_payoff_diminishing_returns) since both
   review checkpoints already catch the bounded over-rename risk.

2. TRANSITIVE SEMANTIC DRIFT NOT CAUGHT. Lines that REFER to per-core framing without
   using a target token ("see CLAUDE.md's sharding model") are invisible to token
   matching. MITIGATION: plan body Phase H.4.1 semantic-context spot-check.

3. DEFAULT BIAS IS RENAME ON NARRATIVE. Under-rename (missed sites) is caught by the
   comprehensive rg pass; over-rename (false positive) is caught by operator TSV review.
   Both directions have a review safety net by design.
"""

import argparse
import os
import re
import sys
from collections import Counter
from datetime import date
from pathlib import Path
from typing import List, NamedTuple, Tuple


# Default token inventory — .D.1 sweeps CONCEPTUAL terminology ONLY.
#
# CODE SYMBOLS (CoreContext / MAX_CORES / state.cores / FOREACH_PER_CORE_CFG_FIELD /
# core_strategy / core_risk_pct / per_core_slow) are deliberately ABSENT. They are CODE
# IDENTIFIERS still named Core*/per_core until v5.15.5.F.4d.1.E.1 — they rename WITH the
# code at .E.1 (doc-citations + code together as a sister-cohort), NOT at this doc sweep.
# Reasons (per .D.1 Phase A proof finding 2026-05-28):
#   1. Renaming `CoreContext`→`NodeContext` in prose NOW cites a symbol that won't exist in
#      code until .E.1 (Class-14-flavored fabricated-symbol-in-docs).
#   2. Produces half-renamed phrases ("cores[16] × NodeContext" — array still `cores`, type
#      now `NodeContext`).
#   3. Underscore/identifier tokens (per_core, MAX_CORES) match INSIDE larger identifiers
#      (FOREACH_PER_CORE_CFG_FIELD, per_core_slow) → substring mangling
#      (sister: feedback_avoid_substring_replace_all_on_member_access + Class 36).
# The conceptual hyphenated `per-core` adjective is safe (hyphens can't be inside a C
# identifier) + is the high-value vision-language reframe ("per-core sharded platform" →
# "per-node sharded platform").
DEFAULT_TOKENS = [
    "per-core",      # conceptual adjective; case-insensitive match covers Per-Core / PER-CORE
    "single_core",   # deprecated-mode concept; .D.1 DELETES refs (manual; not a mechanical rename)
]

# Tokens that LOOK like rename candidates but should be PRESERVED (per plan body Glossary anchor).
# Detection rule: if a hit's matched substring is part of one of these, classify as keep-token.
KEEP_TOKENS = ["ExecutionCore", "CPU core", "isolcpus", "nohz_full"]

# Default exclude patterns. Per feedback_archived_changelog_preservation_discipline +
# feedback_terminology_evolution_bridge_not_history_rewrite: HISTORICAL-RECORD docs
# (postmortems / handoffs / audit reports / archived changelogs / current-changelog rows)
# are PRESERVED truthfully, NOT swept. They describe past states accurately; rewriting
# falsifies the evolution record + breaks the .E.1-rename narrative coherence. The
# old<->new terminology bridge lives in the canonical glossary + dir-banner READMEs,
# not in rewritten history.
DEFAULT_EXCLUDE = [
    "DOCS/changelogs/",          # archived per-sprint changelog write-ups
    "DOCS/CHANGELOG.md",         # current changelog: per-ship rows are historical-once-committed
    "HOT_PATH_CHANGELOG.md",     # changelog: per-entry historical (per archived-changelog-preservation)
    "/legacy/",
    "/_archive/",
    "/postmortems/",             # historical per-ship retrospectives
    "/handoffs/",                # historical session-handoff docs
    "/plan_checks/",             # ephemeral audit-report output
    "/capture-audit-reports/",   # ephemeral audit-report output
    "/decision-logs/",           # historical decision capture (active log gets entries APPENDED, not swept)
    # LEDGERS + code-state inventories: accumulating-historical-record. Their entries are
    # point-in-time findings about per-core CODE that is still per-core until .E.1; they
    # update WITH the code at .E.1, not at the .D.1 doc sweep. Preserve+bridge, don't sweep.
    "PARITY_ISSUES.md",          # parity-finding ledger (entries are point-in-time findings)
    "TECH_DEBT.md",              # tech-debt ledger
    "/tech-debt/",               # tech-debt open.md / closed.md sub-ledgers
    "FEATURE_LOOKUP.md",         # operator-facing feature inventory (tracks current per-core code)
    "MANUAL_FIELDS_INVENTORY.md", # cfg-field inventory (tracks current per-core code)
    "KNOWN_ISSUES.md",           # current-limitations ledger (point-in-time entries)
]

# Default scope = FORWARD-LOOKING timeless canonical docs only.
# - CLAUDE.md / CLAUDE.local.md: always-loaded orientation (engine-repo)
# - DOCS/: architecture + philosophy + operator docs (engine-repo; CHANGELOG excluded above)
# - workspace DESIGN_SPECS/: pattern catalog narrative (workspace-native; absolute path —
#   skipped silently on clean clones without the workspace per is_dir() guard)
# Active .E plan bodies + MASTER + E-MASTER-REFERENCE + running list + decoupling roadmap
# are FORWARD-LOOKING and ARE swept, but via EXPLICIT --scope at the sweep phase (not the
# default) to avoid pulling historical bundled subplans from the same dir.
DEFAULT_SCOPE = [
    "CLAUDE.md",
    "CLAUDE.local.md",
    "DOCS/",
    "/home/caramel/code/tick-trader-percore-workspace/DESIGN_SPECS/",
]


# --apply mode: EXACT case-variant → target map for AUTO-APPLICABLE mechanical renames.
# Only these exact matched strings auto-apply (case-preserving by explicit enumeration —
# safer than algorithmic case-folding for hyphenated/compound tokens). Anything matched but
# NOT in this map is reported as NEEDS-MANUAL (never auto-edited). single_core is
# deliberately ABSENT — its action is "delete/rephrase references," not a mechanical rename.
RENAME_MAP = {
    # CONCEPTUAL hyphenated per-core adjective ONLY (the .D.1 scope). Code-symbol renames
    # (CoreContext→NodeContext, MAX_CORES→MAX_NODES, state.cores→state.nodes,
    # FOREACH_PER_CORE_CFG_FIELD→…, core_strategy→…, per_core→per_node) are DEFERRED to .E.1
    # where they rename alongside the code. single_core is ABSENT (its action is delete/rephrase,
    # handled manually). The overlap-resolution logic below stays as defensive infrastructure
    # for .E.1 reuse (when code-symbol tokens with overlapping spans get added) + Class 36.
    "per-core": "per-node", "Per-core": "Per-node", "Per-Core": "Per-Node", "PER-CORE": "PER-NODE",
}

# Classes whose hits are eligible for auto-apply (the RENAME-action narrative classes).
# code-fence-target (RENAME-WITH-CONFIRM), memory-link-crossref, claude-md-section-crossref,
# and all LEAVE classes are NEVER auto-applied.
AUTO_APPLY_CLASSES = {"narrative-current-state", "narrative-aspirational"}


class Hit(NamedTuple):
    file: str
    line: int
    content: str
    inside_fence: bool
    token: str
    suggested_class: str
    suggested_action: str
    confidence: str  # HIGH / MED / LOW


# Transition-documentation patterns — legitimate rename references that should LEAVE.
# Per .D.1 plan body cycle 1 N2 amendment.
TRANSITION_PATTERNS = [
    re.compile(r"renamed\s+from\s+[^,]*\bat\s+\.\w", re.IGNORECASE),
    re.compile(r"\bold:\s*per[-_]core\s*[→\-]>?\s*new:\s*per[-_]node", re.IGNORECASE),
    re.compile(r"per[-_]node[^\n]*\(was\s+per[-_]core", re.IGNORECASE),
    re.compile(r"per[-_]core[^\n]*\(now\s+per[-_]node", re.IGNORECASE),
    re.compile(r"was\s+per[-_]core;\s*is\s+now\s+per[-_]node", re.IGNORECASE),
    re.compile(r"per[-_]NODE\s+sharded\s+\(was\s+per[-_]core", re.IGNORECASE),
    re.compile(r"Core\s*→\s*Node\s+rename", re.IGNORECASE),
]

# Memory link cross-ref patterns.
# Per .D.1 plan body cycle 1 N8 amendment (handles both [[name]] AND [Title](file.md) AND bare refs).
MEMORY_LINK_PATTERNS = [
    re.compile(r"\[\[([a-z_][a-z0-9_]*)\]\]"),                   # [[name]]
    re.compile(r"\[[^\]]+\]\(([a-z_][a-z0-9_]*\.md)\)"),         # [Title](file.md)
    re.compile(r"\b(feedback_[a-z_]+|user_[a-z_]+|project_[a-z_]+|reference_[a-z_]+)\b"),  # bare ref
]

# CLAUDE.md / DESIGN_PHILOSOPHY / DESIGN_SPECS section cross-ref patterns.
SECTION_XREF_PATTERN = re.compile(
    r"(?:see|per)\s+(?:DOCS/)?(?:DESIGN_PHILOSOPHY|CLAUDE)(?:\.md)?\s*§"
    r"|DESIGN_SPECS/[^\s]+\s*§"
    r"|§\s+\d+",
    re.IGNORECASE,
)

# Ship-tag pattern (v5.X.Y.Z, v0.X.Y, etc.).
SHIP_TAG_PATTERN = re.compile(r"\bv\d+\.\d+(?:\.\d+)?(?:\.[\w.]+)?")

# Historical-tense markers (past tense; describes past state).
HISTORICAL_TENSE_PATTERN = re.compile(
    r"\b(?:landed|shipped|deployed|introduced|deprecated|was|were|had|did|removed at|deleted at|"
    r"closed at|codified at|established at)\b",
    re.IGNORECASE,
)

# Current-changelog-row pattern (per .D.1 plan body cycle 1 N6 amendment).
CHANGELOG_ROW_PATTERN = re.compile(r"^\s*\|\s*\*\*\d+\.\d+", re.IGNORECASE)

# Aspirational markers (vision / end-goal / future).
ASPIRATIONAL_PATTERN = re.compile(
    r"\b(?:vision|end[-_]goal|end[-_]state|future|will\b|going\s+to|target|aim|aspiration|"
    r"\.E\.\d|\.E\.X|sub-sprint|sprint\s+end)\b",
    re.IGNORECASE,
)


_PATH_TOKEN_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./-")
_FILE_EXT_RE = re.compile(r"\.(md|hpp|cpp|py|sh|txt|h|c|yaml|yml|json|cfg)$", re.IGNORECASE)


def is_in_path_like_token(content: str, token_start: int, token_end: int) -> bool:
    """True if the matched per-core span is part of a FILESYSTEM-PATH-LIKE token.

    Per .D.1 Phase A post-write finding (2026-05-28): the doc-rename write renamed `per-core`
    inside file-PATH references (e.g., `DOCS/recurring-bug-patterns/class-25-...-per-core-
    consumer.md`, `type-erased-per-core-resource-handle-pattern.md`), breaking links to the
    actual per-core-NAMED files (which keep their names until .E.1 renames the FILES). File
    paths + filename slugs are STABLE IDENTIFIERS — never rename them in a doc-terminology
    sweep; they rename only when the file itself is deliberately renamed.

    Detection: expand to the maximal run of path-token chars [A-Za-z0-9_./-] containing the
    match; it is path-like if that run contains '/' OR ends in a known file extension.
    """
    lo = token_start
    while lo > 0 and content[lo - 1] in _PATH_TOKEN_CHARS:
        lo -= 1
    hi = token_end
    while hi < len(content) and content[hi] in _PATH_TOKEN_CHARS:
        hi += 1
    token = content[lo:hi]
    if "/" in token:
        return True
    if _FILE_EXT_RE.search(token):
        return True
    return False


def is_in_keep_token_context(content: str, token_start: int, token_end: int) -> bool:
    """Check if the matched token span is actually inside a KEEP_TOKEN (e.g., 'core' inside 'ExecutionCore')."""
    # Expand outward to check ~30 chars on each side
    start = max(0, token_start - 30)
    end = min(len(content), token_end + 30)
    context = content[start:end]
    for keep in KEEP_TOKENS:
        if keep.lower() in context.lower():
            # If keep-token surrounds the matched position, this is a false-positive
            for kmatch in re.finditer(re.escape(keep), context, re.IGNORECASE):
                k_abs_start = start + kmatch.start()
                k_abs_end = start + kmatch.end()
                if k_abs_start <= token_start and token_end <= k_abs_end:
                    return True
    return False


def classify_hit(
    content: str,
    inside_fence: bool,
    file_path: str,
    line_num: int,
    all_lines: List[str],
    token: str,
    token_start: int,
    token_end: int,
) -> Tuple[str, str, str]:
    """Classify a hit per the 12-class matrix.

    Returns: (suggested_class, suggested_action, confidence).
    """
    # 0. Keep-token context — false-positive (token matched inside ExecutionCore / CPU core / etc.)
    if is_in_keep_token_context(content, token_start, token_end):
        return ("keep-token-context", "LEAVE", "HIGH")

    # 0.5. FILE-PATH / FILENAME-SLUG reference — stable identifier; never rename in a
    # terminology sweep (renames only when the FILE itself is renamed at .E.1).
    # Per .D.1 Phase A post-write broken-link finding.
    if is_in_path_like_token(content, token_start, token_end):
        return ("file-path-reference", "LEAVE", "HIGH")

    # 1. ARCHIVED FILES — never touch
    if any(pat in file_path for pat in DEFAULT_EXCLUDE):
        return ("archived-file", "LEAVE", "HIGH")

    # 2. CURRENT CHANGELOG ROWS (per .D.1 cycle 1 N6 amendment)
    if "DOCS/CHANGELOG.md" in file_path and CHANGELOG_ROW_PATTERN.match(content):
        return ("current-changelog-row", "LEAVE", "HIGH")

    # 3. TRANSITION DOCUMENTATION (per .D.1 cycle 1 N2 amendment)
    for pat in TRANSITION_PATTERNS:
        if pat.search(content):
            return ("transition-documentation", "LEAVE", "HIGH")

    # 4. MEMORY LINK CROSSREFS (per .D.1 cycle 1 N7 + N8 amendments)
    for mlp in MEMORY_LINK_PATTERNS:
        match = mlp.search(content)
        if match:
            matched_name = match.group(1) if match.lastindex else ""
            # If the matched name contains a rename-target token, flag as sister-rename candidate
            name_normalized = matched_name.lower().replace("-", "_")
            for tok in DEFAULT_TOKENS:
                tok_normalized = tok.lower().replace("-", "_").replace(".", "_")
                if tok_normalized in name_normalized:
                    return ("memory-link-crossref", "SISTER-RENAME-CANDIDATE", "MED")

    # 5. CLAUDE.md / DESIGN_PHILOSOPHY section crossrefs (per .D.1 cycle 1 N8 amendment)
    if SECTION_XREF_PATTERN.search(content):
        return ("claude-md-section-crossref", "SISTER-COHORT-XREF", "MED")

    # 6. INSIDE CODE FENCE
    if inside_fence:
        # Check preceding 4 lines for target-architecture markers
        start = max(0, line_num - 5)
        context_window = "\n".join(all_lines[start:line_num])
        if re.search(
            r"target\s+architecture|post-\.E\.1|after\s+Core[→\-]>?Node|target\s+shape|TARGET",
            context_window,
            re.IGNORECASE,
        ):
            return ("code-fence-target", "RENAME-WITH-CONFIRM", "MED")
        return ("code-fence-cite", "LEAVE", "HIGH")

    # 7. OUTSIDE CODE FENCE
    # 7a. SHIP-TAG CITATION (token within ~80 chars of v5.X.Y)
    ship_tag_matches = list(SHIP_TAG_PATTERN.finditer(content))
    if ship_tag_matches:
        for sm in ship_tag_matches:
            if abs(sm.start() - token_start) < 80:
                return ("ship-tag-citation", "LEAVE", "HIGH")

    # 7b. HISTORICAL TENSE (past tense + token nearby)
    if HISTORICAL_TENSE_PATTERN.search(content):
        return ("historical-tense", "LEAVE", "MED")

    # 7c. NARRATIVE — split aspirational vs current-state
    if ASPIRATIONAL_PATTERN.search(content):
        return ("narrative-aspirational", "RENAME", "MED")

    return ("narrative-current-state", "RENAME", "MED")


def parse_md_file(file_path: str, tokens: List[str]) -> List[Hit]:
    """Parse a markdown file; return list of hits.

    Deduplication: matching is case-insensitive, so token-list variants ("per-core" /
    "Per-Core" / "PER-CORE") match the same substring. Dedupe tokens by lowercase to
    prevent duplicate hits per (line, matched-text).
    """
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            raw_lines = f.readlines()
    except (IOError, UnicodeDecodeError) as e:
        print(f"WARN: could not read {file_path}: {e}", file=sys.stderr)
        return []

    lines = [l.rstrip("\n") for l in raw_lines]
    hits: List[Hit] = []
    inside_fence = False

    # Fence-robustness WARN (per .D.1 Phase A.5 finding): if a file has an ODD number of
    # ``` fence markers, the open/close pairing is unbalanced — the fence-state tracker
    # desyncs partway through, after which narrative may be wrongly classified
    # code-fence-cite (LEAVE) → under-rename. Emit a per-file WARN so the operator knows
    # this file's code-fence-cite classifications are SUSPECT + need manual verification.
    # (Discovered when .E.1 = 37 fence markers desync'd the tests-section tool's tracker.)
    fence_count = sum(1 for l in lines if l.lstrip().startswith("```"))
    if fence_count % 2 != 0:
        print(
            f"WARN: {file_path} has {fence_count} (ODD) ``` fence markers — fence-state "
            f"tracking may desync; code-fence-cite classifications in this file are SUSPECT, "
            f"verify manually.",
            file=sys.stderr,
        )

    # Dedupe tokens by lowercase (case-insensitive matching means variants collide)
    seen_lc: set = set()
    unique_tokens: List[str] = []
    for t in tokens:
        if t.lower() not in seen_lc:
            seen_lc.add(t.lower())
            unique_tokens.append(t)

    for line_num, line in enumerate(lines, start=1):
        # Track code-fence state (markdown ``` fences)
        if line.lstrip().startswith("```"):
            inside_fence = not inside_fence
            continue

        # Track per-line hits to dedupe (file, line, matched-position) across tokens
        line_hit_positions: set = set()

        for token in unique_tokens:
            token_re = re.compile(re.escape(token), re.IGNORECASE)
            for m in token_re.finditer(line):
                if m.start() in line_hit_positions:
                    continue  # Already recorded a hit at this position
                line_hit_positions.add(m.start())
                suggested_class, suggested_action, confidence = classify_hit(
                    line,
                    inside_fence,
                    file_path,
                    line_num,
                    lines,
                    token,
                    m.start(),
                    m.end(),
                )
                hits.append(
                    Hit(
                        file=file_path,
                        line=line_num,
                        content=line.strip()[:200],
                        inside_fence=inside_fence,
                        token=m.group(),
                        suggested_class=suggested_class,
                        suggested_action=suggested_action,
                        confidence=confidence,
                    )
                )

    return hits


def apply_file(file_path: str, tokens: List[str], write: bool) -> Tuple[int, int, List[str]]:
    """Apply auto-applicable renames to one file.

    Re-parses the file (positions are authoritative), and for each line applies
    substitutions RIGHT-TO-LEFT (so earlier positions don't shift) for hits that are:
      - classified in AUTO_APPLY_CLASSES, AND
      - whose matched text is an exact key in RENAME_MAP.
    Everything else (single_core, not-in-map, confirm, sister, all LEAVE) is left untouched
    and counted as NEEDS-MANUAL.

    Returns (applied_count, needs_manual_count, manual_samples).
    If write=False, computes counts + samples WITHOUT writing (preview/dry-run).
    """
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            raw_lines = f.readlines()
    except (IOError, UnicodeDecodeError) as e:
        print(f"WARN: could not read {file_path}: {e}", file=sys.stderr)
        return (0, 0, [])

    lines = [l.rstrip("\n") for l in raw_lines]

    # Dedupe tokens by lowercase (same as parse_md_file)
    seen_lc: set = set()
    unique_tokens: List[str] = []
    for t in tokens:
        if t.lower() not in seen_lc:
            seen_lc.add(t.lower())
            unique_tokens.append(t)

    applied = 0
    needs_manual = 0
    manual_samples: List[str] = []
    inside_fence = False
    new_lines: List[str] = []

    for line_num, line in enumerate(lines, start=1):
        if line.lstrip().startswith("```"):
            inside_fence = not inside_fence
            new_lines.append(line)
            continue

        # Collect auto-applicable substitutions on this line: (start, end, replacement)
        subs: List[Tuple[int, int, str]] = []
        seen_positions: set = set()
        for token in unique_tokens:
            token_re = re.compile(re.escape(token), re.IGNORECASE)
            for m in token_re.finditer(line):
                if m.start() in seen_positions:
                    continue
                seen_positions.add(m.start())
                cls, action, _conf = classify_hit(
                    line, inside_fence, file_path, line_num, lines, token, m.start(), m.end()
                )
                matched = m.group()
                if cls in AUTO_APPLY_CLASSES and matched in RENAME_MAP:
                    subs.append((m.start(), m.end(), RENAME_MAP[matched]))
                elif cls in AUTO_APPLY_CLASSES or action in ("RENAME", "RENAME-WITH-CONFIRM",
                                                             "SISTER-RENAME-CANDIDATE",
                                                             "SISTER-COHORT-XREF"):
                    # Wanted-to-change-but-can't-auto: needs manual (single_core, not-in-map,
                    # confirm-needed, sister handling)
                    needs_manual += 1
                    if len(manual_samples) < 12:
                        manual_samples.append(
                            f"{file_path}:{line_num} [{cls}/{action}] '{matched}': {line.strip()[:90]}"
                        )

        # Resolve OVERLAPS before applying (per .D.1 Phase A apply-preview finding):
        # e.g. FOREACH_PER_CORE_CFG_FIELD (0,26) and its inner PER_CORE (8,16) both match.
        # Two overlapping subs on one span = corruption if replacement lengths differ.
        # Greedily accept non-overlapping subs preferring the LONGER (outer) match:
        # sort by start asc, then by span length desc; accept if start >= last accepted end.
        if subs:
            subs.sort(key=lambda s: (s[0], -(s[1] - s[0])))
            accepted: List[Tuple[int, int, str]] = []
            last_end = -1
            for start, end, repl in subs:
                if start >= last_end:
                    accepted.append((start, end, repl))
                    last_end = end
                # else: overlaps a longer/earlier accepted sub → skip (inner substring)
            new_line = line
            for start, end, repl in sorted(accepted, key=lambda s: -s[0]):
                new_line = new_line[:start] + repl + new_line[end:]
            new_lines.append(new_line)
            applied += len(accepted)
        else:
            new_lines.append(line)

    if write and applied > 0:
        with open(file_path, "w", encoding="utf-8") as f:
            f.write("\n".join(new_lines))
            if raw_lines and raw_lines[-1].endswith("\n"):
                f.write("\n")

    return (applied, needs_manual, manual_samples)


def find_md_files(scope: List[str], exclude_patterns: List[str]) -> List[str]:
    """Find all .md files in scope; exclude matching patterns."""
    md_files: List[str] = []
    for s in scope:
        s_path = Path(s)
        if s_path.is_file() and s_path.suffix == ".md":
            md_files.append(str(s_path))
        elif s_path.is_dir():
            for md_file in s_path.rglob("*.md"):
                file_str = str(md_file)
                if not any(pat in file_str for pat in exclude_patterns):
                    md_files.append(file_str)
    return sorted(md_files)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Doc rename classification tool (per .D.1 doc-sweep ship Phase A.1)"
    )
    parser.add_argument(
        "--scope",
        nargs="+",
        default=DEFAULT_SCOPE,
        help="Paths to scan (files or directories)",
    )
    parser.add_argument(
        "--tokens",
        type=lambda s: s.split(","),
        default=DEFAULT_TOKENS,
        help="Comma-separated tokens to flag",
    )
    parser.add_argument(
        "--exclude",
        type=lambda s: s.split(","),
        default=DEFAULT_EXCLUDE,
        help="Comma-separated patterns to exclude",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit 1 on any unclassified hit (for pre-commit hook)",
    )
    parser.add_argument(
        "--output",
        default=None,
        help=(
            "Output TSV path; default: "
            "plans/v5.15-live-readiness/plan_checks/D.1-classification-reports/"
            "<YYYY-MM-DD>-doc-rename-classification.tsv"
        ),
    )
    parser.add_argument(
        "--summary-only",
        action="store_true",
        help="Print only summary; don't write TSV (useful for spot-checks)",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Apply auto-applicable renames (narrative classes + exact RENAME_MAP variants). "
        "PREVIEW by default (writes nothing); add --write to actually edit files.",
    )
    parser.add_argument(
        "--write",
        action="store_true",
        help="With --apply: actually write edits (else preview-only).",
    )
    args = parser.parse_args()

    md_files = find_md_files(args.scope, args.exclude)

    # --apply mode (preview unless --write)
    if args.apply:
        total_applied = 0
        total_manual = 0
        all_manual_samples: List[str] = []
        per_file: List[Tuple[str, int, int]] = []
        for md_file in md_files:
            applied, manual, samples = apply_file(md_file, args.tokens, write=args.write)
            if applied or manual:
                per_file.append((md_file, applied, manual))
            total_applied += applied
            total_manual += manual
            all_manual_samples.extend(samples[: max(0, 12 - len(all_manual_samples))])

        mode = "WROTE" if args.write else "PREVIEW (no files written; add --write to apply)"
        print(f"=== --apply {mode} ===", file=sys.stderr)
        print(f"Files scanned: {len(md_files)}", file=sys.stderr)
        print(f"Auto-applied renames: {total_applied}", file=sys.stderr)
        print(f"NEEDS-MANUAL (single_core / not-in-map / confirm / sister): {total_manual}", file=sys.stderr)
        print("\nTop files by auto-applied count:", file=sys.stderr)
        for fp, a, mn in sorted(per_file, key=lambda x: -x[1])[:15]:
            print(f"  {a:4d} applied / {mn:3d} manual  {fp}", file=sys.stderr)
        if all_manual_samples:
            print("\nSample NEEDS-MANUAL hits (handle by hand):", file=sys.stderr)
            for s in all_manual_samples:
                print(f"  - {s}", file=sys.stderr)
        return 0

    all_hits: List[Hit] = []
    for md_file in md_files:
        all_hits.extend(parse_md_file(md_file, args.tokens))

    # Compute output path
    if not args.summary_only:
        if args.output is None:
            today = date.today().strftime("%Y-%m-%d")
            output_dir = "plans/v5.15-live-readiness/plan_checks/D.1-classification-reports"
            os.makedirs(output_dir, exist_ok=True)
            args.output = os.path.join(
                output_dir, f"{today}-doc-rename-classification.tsv"
            )

        with open(args.output, "w", encoding="utf-8") as f:
            f.write(
                "file\tline\tcontent\tinside_fence\ttoken\t"
                "suggested_class\tsuggested_action\tconfidence\n"
            )
            for hit in all_hits:
                content_clean = hit.content.replace("\t", " ").replace("\n", " ")
                f.write(
                    f"{hit.file}\t{hit.line}\t{content_clean}\t"
                    f"{hit.inside_fence}\t{hit.token}\t{hit.suggested_class}\t"
                    f"{hit.suggested_action}\t{hit.confidence}\n"
                )

    # Summary to stderr
    print(f"Scanned: {len(md_files)} .md files", file=sys.stderr)
    print(f"Total hits: {len(all_hits)}", file=sys.stderr)

    by_class = Counter(h.suggested_class for h in all_hits)
    print("By class:", file=sys.stderr)
    for cls, n in sorted(by_class.items(), key=lambda x: -x[1]):
        print(f"  {cls}: {n}", file=sys.stderr)

    by_action = Counter(h.suggested_action for h in all_hits)
    print("By action:", file=sys.stderr)
    for act, n in sorted(by_action.items(), key=lambda x: -x[1]):
        print(f"  {act}: {n}", file=sys.stderr)

    if not args.summary_only:
        print(f"Output: {args.output}", file=sys.stderr)

    # Strict mode for pre-commit hook
    if args.strict:
        # Strict means: any unclassified hits (no class) OR ambiguous classifications fail
        ambiguous = [h for h in all_hits if h.suggested_class in ("ambiguous", "")]
        if ambiguous:
            print(f"STRICT MODE: {len(ambiguous)} unclassified hits", file=sys.stderr)
            for h in ambiguous[:5]:
                print(f"  {h.file}:{h.line}: {h.content[:100]}", file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
