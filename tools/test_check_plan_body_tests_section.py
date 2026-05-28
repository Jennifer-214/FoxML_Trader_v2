#!/usr/bin/env python3
"""Unit tests for check_plan_body_tests_section.py (Check 45). Run: python3 -m pytest OR python3 this_file.py"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_plan_body_tests_section as mod  # noqa: E402


def _check(body: str):
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as f:
        f.write(body)
        path = f.name
    try:
        return mod.check_plan_body(path)
    finally:
        os.unlink(path)


def test_not_triggered_no_test_refs():
    verdict, _ = _check("# Plan\n\nThis plan touches DOCS/ only.\n")
    assert verdict == "PASS", verdict


def test_not_triggered_running_binary_not_modifying():
    # "controller_test" (binary, no .cpp) + "tests pass" = execution, NOT modification
    verdict, _ = _check("# Plan\n\nTests pass (3239 controller_test baseline preserved).\n")
    assert verdict == "PASS", verdict


def test_not_triggered_template_placeholder():
    # tests/<file>.cpp with angle brackets = template placeholder, must NOT trigger
    verdict, _ = _check("# Plan\n\nFormat: `tests/<file>.cpp:<line>` — modified tests.\n")
    assert verdict == "PASS", verdict


def test_not_triggered_reorg_discussion():
    # tests/{unit,...} reorg discussion (D-36) is NOT a modification (dropped trigger)
    verdict, _ = _check("# Plan\n\nFuture: tests/{unit,integration,chaos}/ reorg per D-36.\n")
    assert verdict == "PASS", verdict


def test_triggered_named_test_cpp_missing_section():
    verdict, _ = _check("# Plan\n\nBatch 8: rename controller_test.cpp symbols.\n")
    assert verdict == "VIOLATION-MISSING-SECTION", verdict


def test_triggered_tests_path_missing_section():
    verdict, _ = _check("# Plan\n\nEdit tests/integration_test.hpp for new fixture.\n")
    assert verdict == "VIOLATION-MISSING-SECTION", verdict


def test_triggered_with_complete_section():
    body = (
        "# Plan\n\nModify controller_test.cpp.\n\n"
        "## Tests changed\n\n"
        "### (a) Modified tests\n- foo\n\n"
        "### (b) Broken / replaced tests\n- none\n\n"
        "### (c) NEW unit tests added\n- bar\n"
    )
    verdict, _ = _check(body)
    assert verdict == "PASS", verdict


def test_triggered_section_missing_subcategory():
    body = (
        "# Plan\n\nModify controller_test.cpp.\n\n"
        "## Tests changed\n\n"
        "### (a) Modified tests\n- foo\n\n"
        "### (b) Broken / replaced tests\n- none\n"
        # missing (c)
    )
    verdict, detail = _check(body)
    assert verdict == "VIOLATION-INCOMPLETE-SUBCATEGORIES", verdict
    assert "(c)" in detail, detail


def test_parity_harness_triggers():
    verdict, _ = _check("# Plan\n\nUpdate parity_harness.cpp comparator.\n")
    assert verdict == "VIOLATION-MISSING-SECTION", verdict


def test_unbalanced_fences_dont_hide_trigger():
    # Regression: unbalanced ``` must NOT cause real content after it to be missed
    # (fence-stripping was removed at .D.1 Phase A.5 precisely for this).
    body = (
        "# Plan\n\n"
        "```bash\nsome command\n```\n"
        "```\nunclosed fence opens here\n"  # odd fence count = unbalanced
        "Later: modify controller_test.cpp\n"
    )
    verdict, _ = _check(body)
    assert verdict == "VIOLATION-MISSING-SECTION", verdict


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
    print(f"\n{passed}/{len(tests)} passed")
    return 0 if passed == len(tests) else 1


if __name__ == "__main__":
    sys.exit(run_all())
