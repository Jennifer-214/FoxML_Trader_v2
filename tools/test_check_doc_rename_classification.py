#!/usr/bin/env python3
"""Unit tests for check_doc_rename_classification.py. Run: python3 this_file.py"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_doc_rename_classification as mod  # noqa: E402


def _classify_single(body: str, file_path: str = "DOCS/test.md"):
    """Write body to a temp .md, parse, return list of (token, class, action) hits."""
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False, dir="/tmp") as f:
        f.write(body)
        tmp = f.name
    try:
        hits = mod.parse_md_file(tmp, mod.DEFAULT_TOKENS)
        return [(h.token, h.suggested_class, h.suggested_action) for h in hits]
    finally:
        os.unlink(tmp)


def test_narrative_renames():
    hits = _classify_single("The per-core sharded engine distributes risk.\n")
    assert any(c == "narrative-current-state" and a == "RENAME" for _, c, a in hits), hits


def test_code_fence_cite_left():
    body = "Currently:\n```cpp\nstate.cores[i].field = x;\n```\n"
    hits = _classify_single(body)
    assert any(c == "code-fence-cite" and a == "LEAVE" for _, c, a in hits), hits


def test_execution_core_keep_token():
    # 'core' inside ExecutionCore must NOT be flagged for rename
    hits = _classify_single("Call ExecutionCore_SetParameters per cycle.\n")
    # Either no per-core hit, or classified keep-token-context LEAVE
    bad = [h for h in hits if h[2] == "RENAME"]
    assert not bad, f"ExecutionCore should not yield RENAME: {bad}"


def test_transition_documentation_left():
    hits = _classify_single("per-NODE sharded (was per-core in pre-.E).\n")
    assert any(c == "transition-documentation" and a == "LEAVE" for _, c, a in hits), hits


def test_ship_tag_citation_left():
    hits = _classify_single("v5.10 introduced the per-core architecture.\n")
    # past tense + ship tag → LEAVE (historical or ship-tag-citation)
    assert all(a == "LEAVE" for _, c, a in hits if "per-core" in _.lower()), hits


def test_dedup_case_variants():
    # "per-core" + "Per-Core" on same line at different positions = 2 distinct hits,
    # but the SAME position must not double-count across case-variant tokens.
    hits = _classify_single("per-core and Per-Core both appear.\n")
    # 2 distinct positions → 2 hits (not 4 from case-variant token list)
    per_core_hits = [h for h in hits if h[0].lower() == "per-core"]
    assert len(per_core_hits) == 2, f"expected 2 distinct-position hits, got {len(per_core_hits)}: {hits}"


def test_archived_file_left():
    hits = mod_classify_path("DOCS/changelogs/2026-04-old.md", "The per-core engine.\n")
    assert all(a == "LEAVE" for _, c, a in hits), hits


def mod_classify_path(file_path: str, body: str):
    """Helper: classify with a SPECIFIC file path (for archived-file / changelog tests)."""
    import re

    lines = body.splitlines()
    results = []
    inside_fence = False
    for ln, line in enumerate(lines, 1):
        if line.lstrip().startswith("```"):
            inside_fence = not inside_fence
            continue
        for tok in mod.DEFAULT_TOKENS:
            for m in re.finditer(re.escape(tok), line, re.IGNORECASE):
                cls, act, conf = mod.classify_hit(line, inside_fence, file_path, ln, lines, tok, m.start(), m.end())
                results.append((m.group(), cls, act))
                break
    return results


def test_current_changelog_row_left():
    hits = mod_classify_path("DOCS/CHANGELOG.md", "| **5.10.0** | 2026 | per-core sharding landed |\n")
    assert all(a == "LEAVE" for _, c, a in hits), hits


def run_all():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    passed = 0
    for t in tests:
        try:
            t()
            print(f"  ✅ {t.__name__}")
            passed += 1
        except AssertionError as e:
            print(f"  ❌ {t.__name__}: {e}")
        except Exception as e:  # noqa: BLE001
            print(f"  ⚠️  {t.__name__}: {type(e).__name__}: {e}")
    print(f"\n{passed}/{len(tests)} passed")
    return 0 if passed == len(tests) else 1


if __name__ == "__main__":
    sys.exit(run_all())
