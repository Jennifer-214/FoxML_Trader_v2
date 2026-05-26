#!/usr/bin/env python3
"""check_plan_body_symbol_existence.py — compile-time verification of plan body code samples.

Enforces `feedback_verify_symbol_existence_at_plan_drafting_time` (v1.5 codification;
Stage 6 promotion at v1.7 D3) + `feedback_enumerate_helper_signature_args_before_extract`
(v1.7.3 M6 codification) at COMMIT layer via compile-time verification.

For each plan body markdown file:
1. Extract ```cpp code blocks (treat as executable spec)
2. For each block: try to compile in a test harness with project's actual flags
3. Report compile failures with original plan body file:line citations
4. Exit 0 on success; 1 on any fabrication

Catches:
- Symbol existence (compile fails if symbol doesn't exist) — Class 14
- Type-context (compile fails if pointer-vs-value mismatch) — v1.7.3 NEW-4
- Path existence (compile fails if #include path wrong) — v1.7.3 NEW-1
- Macro signatures (compile fails if macro arg count wrong) — v1.7.3 N-2

Sister to:
- tools/check_doc_metadata.py (YAML frontmatter discipline; sister CI tool shape)
- tools/check_meta_registry.py (H15 FOREACH_REGISTRY topology)
- /readiness Check 33 (body-content enumeration completeness; calls this tool)
- /readiness Check 32 (fabricated-symbol grep-verify; sister coarser check)

Per .B.4 v1.7.4 cycle B-Plus structural enforcement landing (was Phase D D.6 scope;
moved per recurrence-evidence at v1.7.3 → v1.7.4 cycle where M6 codification + memory
alone failed to prevent in-cycle recurrence of 4 NEW Class 14 fabrications in Step C.4
BACKTEST caller code block).

Exit codes:
  0 = all code blocks compile (no fabrications detected)
  1 = at least one fabrication / compile failure
  2 = script error / missing dependencies (g++ / project headers)

Usage:
  python3 tools/check_plan_body_symbol_existence.py <plan-body.md> [more.md ...]
  python3 tools/check_plan_body_symbol_existence.py --all   # check all plans/
  python3 tools/check_plan_body_symbol_existence.py --test-fixtures  # run known-fabrication test suite
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ENGINE = Path("/home/caramel/code/FoxML_Trader_v2")
WORKSPACE = Path("/home/caramel/code/tick-trader-percore-workspace")
PLANS_DIR = WORKSPACE / "plans"

# Compiler flags matching project ./build.sh test build
COMPILER = "g++"
CXX_FLAGS = ["-std=c++20", f"-I{ENGINE}", "-DENGINE_VERSION=\"\""]

# Auto-include shim for code-block compilation — most plan body cpp blocks
# elide standard project headers (ControllerConfig + ControllerEventLoop +
# EngineCommon + FixedPoint + BitmapMacros). Inject these so code blocks
# don't need to re-spell every #include for the tool to compile them.
AUTO_INCLUDE_SHIM = """
#include "CoreFrameworks/EngineCommon.hpp"
#include "CoreFrameworks/EngineSharded.hpp"
#include "DataStream/BinanceCrypto.hpp"
"""

# Code-block extraction patterns
CPP_BLOCK_RE = re.compile(r'```cpp\n(.*?)\n```', re.DOTALL)
INCLUDE_RE = re.compile(r'#include\s+["<][^">]+[">]')

# Heuristic: code blocks shorter than this are usually one-liners / fragments
# not meant to be full compilable units (e.g., signature snippets, single
# expressions). Wrap them in a function shim before compile.
MIN_FULL_TU_LINES = 10


def extract_cpp_blocks(plan_text):
    """Yield (line_number, code_block_text) for each ```cpp block in plan body."""
    line_idx = {}
    cur_line = 1
    for i, ch in enumerate(plan_text):
        line_idx[i] = cur_line
        if ch == '\n':
            cur_line += 1
    for match in CPP_BLOCK_RE.finditer(plan_text):
        start = match.start()
        body = match.group(1)
        yield (line_idx.get(start, '?'), body)


def looks_like_full_tu(code):
    """Heuristic: full translation unit (has #include or function defn)."""
    if INCLUDE_RE.search(code):
        return True
    if re.search(r'^\s*(int|void|template)\s+\w+\s*\(', code, re.MULTILINE):
        return True
    if code.count('\n') >= MIN_FULL_TU_LINES:
        return True
    return False


def wrap_fragment(code):
    """Wrap a code fragment in a function shim for compilability check.

    Common pattern: code blocks contain helper invocations / variable
    declarations / lambda bodies that aren't standalone TUs. Wrap in a
    template void check<unsigned F>() to give them context. Use F=64 (the
    canonical instantiation per FoxML_v2 codebase convention).
    """
    return f"""{AUTO_INCLUDE_SHIM}
namespace tt {{
template <unsigned F>
inline void __plan_body_check__(
    const ControllerConfig<F>& cfg,
    int c,
    EventLoopState<F>& state,
    OrderManagerState<F>& oms,
    int num_cores,
    int tick_index)
{{
    // Common variables that code-block fragments might reference; if unused,
    // compiler warns (we suppress) — the GOAL is to surface fabrications, not
    // to validate the code is sane.
    (void)num_cores; (void)tick_index;
    [[maybe_unused]] FPN<F> price = FPN_Zero<F>();
    [[maybe_unused]] FPN<F> volume = FPN_Zero<F>();
    [[maybe_unused]] uint64_t ts_us = 0;
    [[maybe_unused]] uint64_t now_tick = 0;
    [[maybe_unused]] FPN<F> mtm_price = FPN_Zero<F>();
    [[maybe_unused]] double price_d = 0.0, default_per_core = 0.0, default_risk = 0.0, total_balance = 0.0, core_balance = 0.0;
    [[maybe_unused]] double book_spread_d = 0.0, book_mid_d = 0.0;
    [[maybe_unused]] FPN<F> book_imb = FPN_Zero<F>();
    [[maybe_unused]] BookSnapshot<F> depth = BookSnapshot_Init<F>();
    [[maybe_unused]] CoreModelZoo<F>* zoo_ptr = nullptr;
    [[maybe_unused]] EnsembleModelZoo<F>* ezoo_ptr = nullptr;
    [[maybe_unused]] SPSCRing<Tick<F>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    [[maybe_unused]] ExecutionCore<F> core;
    // BACKTEST-specific: ShardedBacktestDriver<F> for v1.7.4 caller code blocks.
    // (NULL ptr is fine; we only need symbol resolution + type-context check.)
    [[maybe_unused]] ShardedBacktestDriver<F>* drv = nullptr;
    [[maybe_unused]] Tick<F> tick = {{}};
{{
{code}
}}
}}
}}  // namespace tt

// Force instantiation at F=64 to trigger body type-check (canonical FoxML_v2 F).
template void tt::__plan_body_check__<64>(
    const ControllerConfig<64>&,
    int,
    EventLoopState<64>&,
    OrderManagerState<64>&,
    int,
    int);
int main() {{ return 0; }}
"""


def wrap_full_tu(code):
    """Wrap an already-complete-looking TU with project includes."""
    return f"""{AUTO_INCLUDE_SHIM}
{code}
"""


def try_compile(code_str, label):
    """Try to compile code_str. Return (success, stderr)."""
    with tempfile.NamedTemporaryFile(
        mode='w', suffix='.cpp', delete=False, dir=str(ENGINE)
    ) as f:
        f.write(code_str)
        f.flush()
        src_path = Path(f.name)
    try:
        result = subprocess.run(
            [COMPILER, *CXX_FLAGS, "-c", str(src_path), "-o", "/dev/null"],
            capture_output=True, text=True, timeout=60, cwd=str(ENGINE)
        )
        return (result.returncode == 0, result.stderr)
    except subprocess.TimeoutExpired:
        return (False, f"[timeout] compile of {label} exceeded 60s")
    finally:
        try:
            src_path.unlink()
        except OSError:
            pass


def check_plan_body(plan_path):
    """Return (n_blocks, n_failed, [(line, block_excerpt, stderr), ...])."""
    text = plan_path.read_text(encoding='utf-8', errors='replace')
    blocks = list(extract_cpp_blocks(text))
    failures = []
    for (line, body) in blocks:
        if looks_like_full_tu(body):
            wrapped = wrap_full_tu(body)
        else:
            wrapped = wrap_fragment(body)
        ok, stderr = try_compile(wrapped, f"{plan_path.name}:line{line}")
        if not ok:
            excerpt = body.split('\n')[0][:80]
            failures.append((line, excerpt, stderr))
    return (len(blocks), len(failures), failures)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("paths", nargs="*", help="plan body .md files to check")
    p.add_argument("--all", action="store_true",
                   help="check all .md under plans/")
    p.add_argument("--test-fixtures", action="store_true",
                   help="run known-fabrication test suite (for tool validation)")
    p.add_argument("--quiet", action="store_true",
                   help="only print failures + summary")
    args = p.parse_args()

    if args.test_fixtures:
        print("[test-fixtures] Not yet implemented; will land at .B.4 ship close per Phase D Step D.10.5", file=sys.stderr)
        sys.exit(2)

    if args.all:
        paths = sorted(PLANS_DIR.rglob("*.md"))
    else:
        paths = [Path(p) for p in args.paths]

    if not paths:
        print("usage: check_plan_body_symbol_existence.py <plan-body.md> [...] | --all", file=sys.stderr)
        sys.exit(2)

    total_blocks = 0
    total_failed = 0
    any_failure = False
    for path in paths:
        if not path.exists():
            print(f"[error] not found: {path}", file=sys.stderr)
            any_failure = True
            continue
        n_blocks, n_failed, failures = check_plan_body(path)
        total_blocks += n_blocks
        total_failed += n_failed
        if n_failed > 0:
            any_failure = True
            print(f"\n=== {path}  ({n_failed} of {n_blocks} blocks failed) ===", file=sys.stderr)
            for (line, excerpt, stderr) in failures:
                print(f"\n  {path.name}:line~{line}  block excerpt: {excerpt!r}", file=sys.stderr)
                # Only show the first ~15 lines of stderr to keep output bounded
                stderr_lines = stderr.split('\n')[:15]
                for sl in stderr_lines:
                    print(f"    {sl}", file=sys.stderr)
                if len(stderr.split('\n')) > 15:
                    print(f"    [... {len(stderr.split(chr(10))) - 15} more lines truncated]", file=sys.stderr)
        elif not args.quiet:
            print(f"[ok] {path}  ({n_blocks} blocks compile)")

    print(f"\n=== SUMMARY: {total_blocks} blocks checked across {len(paths)} files; {total_failed} failed ===", file=sys.stderr)
    sys.exit(1 if any_failure else 0)


if __name__ == "__main__":
    main()
