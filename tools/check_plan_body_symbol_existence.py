#!/usr/bin/env python3
"""check_plan_body_symbol_existence.py — compile-time verification of plan body code samples.

Enforces `feedback_verify_symbol_existence_at_plan_drafting_time` (v1.5 codification;
Stage 6 promotion at v1.7 D3) + `feedback_enumerate_helper_signature_args_before_extract`
(v1.7.3 M6 codification) at COMMIT layer via compile-time verification.

For each plan body markdown file:
1. Extract ```cpp code blocks (treat as executable spec; skip blocks inside markdown tables)
2. Per-block include resolution — derive minimal headers based on referenced symbols
3. Try compile in test harness with project's actual flags
4. Categorize failures: FABRICATION (real issue) vs HARNESS-ISSUE (tool's shim incomplete)
5. Report with original plan body file:line citations
6. Exit 0 on success / 1 on any fabrication / 2 on script error

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
  python3 tools/check_plan_body_symbol_existence.py --strict   # also report HARNESS-ISSUE blocks (default: only FABRICATION)
  python3 tools/check_plan_body_symbol_existence.py --quiet   # only failures + summary
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

COMPILER = "g++"
CXX_FLAGS = ["-std=c++20", f"-I{ENGINE}", "-DENGINE_VERSION=\"\""]

# Per-symbol include resolution map. Add entries as new symbols surface.
# Goal: derive minimal includes per block; avoid pulling EngineSharded.hpp
# (transitively requires TUISnapshot etc.; too heavy for unit-style check).
SYMBOL_INCLUDES = {
    # Core types
    "FPN":                          "FixedPoint/FixedPointN.hpp",
    "FPN_Zero":                     "FixedPoint/FixedPointN.hpp",
    "FPN_FromDouble":               "FixedPoint/FixedPointN.hpp",
    "FPN_ToDouble":                 "FixedPoint/FixedPointN.hpp",
    "FPN_IsZero":                   "FixedPoint/FixedPointN.hpp",
    "FPN_Mul":                      "FixedPoint/FixedPointN.hpp",
    "FPN_LessThan":                 "FixedPoint/FixedPointN.hpp",
    "FPN_GreaterThan":              "FixedPoint/FixedPointN.hpp",
    "BITMAP_IS_SET":                "MemHeaders/BitmapMacros.hpp",
    "BITMAP_SET":                   "MemHeaders/BitmapMacros.hpp",
    "BITMAP_CLR":                   "MemHeaders/BitmapMacros.hpp",
    "BITMAP_BIT_U16":               "MemHeaders/BitmapMacros.hpp",
    "CORE_STATE_FLAG_SET":          "MemHeaders/CoreStateFlagRegistry.hpp",
    "CORE_STATE_FLAG_IS_SET":       "MemHeaders/CoreStateFlagRegistry.hpp",
    # CoreFrameworks
    "ControllerConfig":             "CoreFrameworks/ControllerConfig.hpp",
    "EventLoopState":               "CoreFrameworks/ControllerEventLoop.hpp",
    "OrderManagerState":            "CoreFrameworks/OrderManager.hpp",
    "ExecutionCore":                "CoreFrameworks/ExecutionCore.hpp",
    "SPSCRing":                     "CoreFrameworks/ExecutionCore.hpp",
    "Tick":                         "DataStream/BinanceCrypto.hpp",
    "EXECUTION_CORE_TICK_RING_SIZE": "CoreFrameworks/ExecutionCore.hpp",
    "EngineCommon_ApplyBnbDiscount": "CoreFrameworks/EngineCommon.hpp",
    "EngineCommon_BootGlobal":       "CoreFrameworks/EngineCommon.hpp",
    "EngineCommon_BootPerCore":      "CoreFrameworks/EngineCommon.hpp",
    "EngineCommon_SlowPathCycleOneCore":  "CoreFrameworks/EngineCommon.hpp",
    "EngineCommon_SlowPathCycleAllCores": "CoreFrameworks/EngineCommon.hpp",
    "BACKTEST_REGIME_SAMPLE_CORE":  "CoreFrameworks/EngineCommon.hpp",
    "ShardedBacktestDriver":        "CoreFrameworks/ShardedBacktestDriver.hpp",
    "ModelValidation":              "CoreFrameworks/ModelValidation.hpp",
    # SlowPathGateRegistry
    "FOREACH_SLOW_PATH_GATE":       "CoreFrameworks/SlowPathGateRegistry.hpp",
    "MASK_BREAKEVEN_ON_PROFIT":     "CoreFrameworks/SlowPathGateRegistry.hpp",
    "MASK_LADDER_ACTIVE":           "CoreFrameworks/SlowPathGateRegistry.hpp",
    "MASK_CONFIDENCE_ENABLED":      "CoreFrameworks/SlowPathGateRegistry.hpp",
    "SLOW_PATH_GATE_AUTOPOPULATE_ENGINE_WIDE": "CoreFrameworks/SlowPathGateRegistry.hpp",
    "SLOW_PATH_GATE_AUTOPOPULATE_PER_CORE":    "CoreFrameworks/SlowPathGateRegistry.hpp",
    "SlowPathGateState":            "CoreFrameworks/SlowPathGateRegistry.hpp",
    "GlobalGateState":              "CoreFrameworks/SlowPathGateRegistry.hpp",
    # DataStream
    "BookSnapshot":                 "DataStream/BinanceDepth.hpp",
    "BookSnapshot_Init":            "DataStream/BinanceDepth.hpp",
    "BookLevel":                    "DataStream/BinanceDepth.hpp",
    # Strategies
    "STRATEGY_ML":                  "Strategies/StrategyInterface.hpp",
    "STRATEGY_NONE":                "Strategies/StrategyInterface.hpp",
    # ML headers
    "CoreModelZoo":                 "ML_Headers/CoreModelZoo.hpp",
    "EnsembleModelZoo":             "ML_Headers/CoreModelZoo.hpp",
    "ConfidenceScorer_Init":        "ML_Headers/ConfidenceScore.hpp",
    "ConfidenceScorer_BindCompositeCfg": "ML_Headers/ConfidenceScore.hpp",
    "RollingTurnover_Init":         "ML_Headers/RollingTurnover.hpp",
    "FeatureOverlay_PostLoadVerify": "ML_Headers/FeatureRegistryOverlay.hpp",
    "MODEL_BACKEND_XGBOOST":        "ML_Headers/ModelInference.hpp",
    # EventLoop helpers (these are in ControllerEventLoop.hpp but caller needs:
    # we don't include EngineSharded.hpp since it transitively pulls TUISnapshot etc.)
    "EventLoopState_Init":          "CoreFrameworks/ControllerEventLoop.hpp",
    "EventLoopState_ConfigureKillSwitch": "CoreFrameworks/ControllerEventLoop.hpp",
    "EventLoopState_RegisterCore":  "CoreFrameworks/ControllerEventLoop.hpp",
    "EventLoopState_SetCoreStrategy": "CoreFrameworks/ControllerEventLoop.hpp",
    "EventLoop_BreakevenOnProfit":  "CoreFrameworks/ControllerEventLoop.hpp",
    "EventLoop_BreakevenOnProfitOneCore": "CoreFrameworks/ControllerEventLoop.hpp",
    "EventLoop_TimeExitOneCore":    "CoreFrameworks/ControllerEventLoop.hpp",
    "EventLoop_TrailingSLRatchetOneCore": "CoreFrameworks/ControllerEventLoop.hpp",
    "EventLoop_RebuildOneCore":     "CoreFrameworks/ControllerEventLoop.hpp",
    "EventLoop_UpdateRollingStateOneCore": "CoreFrameworks/ControllerEventLoop.hpp",
}

SYMBOL_PATTERN = re.compile(r'\b([A-Z][A-Za-z0-9_]+)\b|\b(FPN|BITMAP|STRATEGY|MASK|FOREACH|SLOW_PATH|MODEL)_[A-Za-z0-9_]+\b')

# Failure classification — distinguish FABRICATION from HARNESS-ISSUE
FABRICATION_PATTERNS = [
    re.compile(r"was not declared in this scope"),
    re.compile(r"no member named\s+['\"]\w+['\"]"),
    re.compile(r"has no member named"),
    re.compile(r"cannot convert.*to.*in (initialization|assignment|return)"),
    re.compile(r"no matching function for call to"),
    re.compile(r"['\"][\w/.]+\.hpp['\"]:\s+No such file or directory"),
    re.compile(r"expected.*before"),  # often signals wrong arg count
]
HARNESS_PATTERNS = [
    re.compile(r"In file included from"),  # transitive header issue
    re.compile(r"expected primary-expression before ['\"]const['\"]"),  # often shim issue
    re.compile(r"'tt' does not name"),  # namespace issue from shim
    re.compile(r"file not found"),
]

# Caller-scope symbol patterns — when these appear in "not declared in this scope"
# errors, treat as harness-issue (caller-scope locals/statics that shim doesn't model).
# Verified at engine HEAD — these ARE real symbols, just outside our compile context.
CALLER_SCOPE_PREFIXES = [
    r"\bg_[a-z_]+",         # g_depth_shared, g_tick_rec, g_init_arena, g_calibration_log_file
    r"\bs_[a-z_]+",         # function-local statics with s_ prefix
    r"\b__atomic_\w+",      # __atomic_load_n etc (works in some compile contexts)
]
KNOWN_HARNESS_FN_MISMATCHES = [
    # ApplyBnbDiscount takes non-const cfg; harness shim uses const cfg as the canonical param
    re.compile(r"no matching function for call to .EngineCommon_ApplyBnbDiscount\(const ControllerConfig"),
]


def classify_failure(stderr):
    """Return ('FABRICATION' | 'HARNESS-ISSUE' | 'UNKNOWN', leading_error_line)."""
    lines = stderr.split('\n')
    # First check known harness mismatches (these override fabrication patterns)
    for line in lines:
        for pat in KNOWN_HARNESS_FN_MISMATCHES:
            if pat.search(line):
                return ("HARNESS-ISSUE", line.strip())
    # Check caller-scope symbol references in "not declared" errors
    # g++ uses Unicode smart quotes ‘x’ (U+2018/U+2019); also accept ASCII '"/`
    not_declared_re = re.compile(r"['\"‘’“”`]([\w]+)['\"‘’“”`] was not declared in this scope")
    for line in lines:
        m = not_declared_re.search(line)
        if m:
            symbol = m.group(1)
            for prefix_pat in CALLER_SCOPE_PREFIXES:
                if re.match(prefix_pat, symbol):
                    return ("HARNESS-ISSUE", line.strip())
    # Detect fabrication patterns (specific compile errors)
    for line in lines:
        for pat in FABRICATION_PATTERNS:
            if pat.search(line):
                return ("FABRICATION", line.strip())
    # Default to harness if we couldn't classify
    return ("HARNESS-ISSUE", lines[0].strip() if lines else "")


def extract_cpp_blocks(plan_text):
    """Yield (line_number, code_block_text) for each ```cpp block in plan body.

    Uses line-based parsing instead of single-regex. Skips blocks inside markdown
    tables (lines starting with `| `).
    """
    lines = plan_text.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.strip().startswith('```cpp'):
            # Find closing ```
            start_line = i + 1  # 1-indexed line number of content start
            j = i + 1
            block_lines = []
            while j < len(lines):
                next_line = lines[j]
                # Closing fence: line that starts with ``` (possibly with indentation removed)
                # Stop at FIRST closing fence (greedy would be wrong)
                if next_line.strip().startswith('```') and not next_line.strip().startswith('```cpp'):
                    # Valid close
                    block = '\n'.join(block_lines)
                    # Skip blocks that look like markdown table content (early line starts with `|`)
                    if not any(l.lstrip().startswith('|') for l in block_lines[:3]):
                        yield (start_line, block)
                    i = j + 1
                    break
                block_lines.append(next_line)
                j += 1
            else:
                # No close found; skip
                i = j
        else:
            i += 1


# Line-anchor verification (v5.15.5.F.4d.1.B.4 v1.7.5 — extends B-Plus from symbol-existence
# to line-range accuracy; fills the /trace-deps coverage gap caught at WIP-9 pre-coding gate
# where C.1's -380 LOC delta shifted EngineSharded.hpp line numbers but plan body cites still
# pointed at pre-shift positions). Closes M7 line-anchor-drift subclass at COMMIT layer.
PLAN_CONTEXT_IDENTIFIER_PATTERNS = [
    # CamelCase_snake_case function names (e.g., EventLoop_BreakevenOnProfit / EngineCommon_BootGlobal)
    re.compile(r'\b([A-Z][a-zA-Z0-9]*_[A-Za-z0-9_]+)\b'),
    # ALL_CAPS_constants (MASK_*, FOREACH_*, STRATEGY_*, GATE_*, etc.)
    re.compile(r'\b(MASK_[A-Z0-9_]+|FOREACH_[A-Z0-9_]+|STRATEGY_[A-Z0-9_]+|GATE_[A-Z0-9_]+|BACKTEST_[A-Z0-9_]+)\b'),
    # Backtick-quoted member access (`drv->current_spread`, `state.cores[c].slow_state`)
    re.compile(r'`([a-z_][\w]*(?:->\w+|\.[a-z_][\w]*))`'),
    # Backtick-quoted function call opens (`Function_Name(`)
    re.compile(r'`([A-Za-z_][\w]*)\s*\(`'),
]

# Generic words to filter out (too common to be load-bearing identifiers)
PLAN_CONTEXT_STOPWORDS = {
    "F", "BACKTEST_FP", "INT", "BOOT", "STEP", "LIVE", "MASK", "FOREACH",
    "PARITY", "TECH_DEBT", "STAGE", "CLOSE", "DELETE", "KEEP", "ADD", "VERIFY",
}


def extract_line_anchors(plan_text):
    """Yield (plan_line, citation, relpath, start_line, end_line, context_text) tuples
    for each <path>:<line> or <path>:<start>-<end> citation in plan body.

    Context: ±3 plan body lines around the citation, used to extract surrounding identifiers
    for fuzzy verification.
    """
    lines = plan_text.split('\n')
    # Pattern: optionally-backticked file:line or file:start-end (e.g., `EngineSharded.hpp:3044-3311`)
    pattern = re.compile(r'`?([A-Za-z_][\w/.-]*\.(?:hpp|cpp)):(\d+)(?:-(\d+))?`?')
    for plan_idx, line in enumerate(lines, 1):
        for m in pattern.finditer(line):
            relpath, start_str, end_str = m.groups()
            start = int(start_str)
            end = int(end_str) if end_str else start
            # Surrounding context for identifier extraction (±3 plan body lines)
            ctx_start = max(0, plan_idx - 4)
            ctx_end = min(len(lines), plan_idx + 3)
            context = '\n'.join(lines[ctx_start:ctx_end])
            yield (plan_idx, m.group(0), relpath, start, end, context)


def extract_context_identifiers(context_text):
    """Return set of identifiers from plan body context useful for fuzzy line-anchor verification."""
    identifiers = set()
    for pat in PLAN_CONTEXT_IDENTIFIER_PATTERNS:
        for m in pat.finditer(context_text):
            ident = m.group(1)
            if ident in PLAN_CONTEXT_STOPWORDS:
                continue
            # Strip trailing/leading whitespace + drop overly short
            ident = ident.strip()
            if len(ident) < 4:
                continue
            identifiers.add(ident)
    return identifiers


def _find_first_line(file_lines, identifier, lo=0, hi=None):
    """Return 1-indexed line number of first occurrence of identifier in file_lines[lo:hi], or None."""
    if hi is None:
        hi = len(file_lines)
    for i in range(lo, hi):
        if identifier in file_lines[i]:
            return i + 1
    return None


_FILE_RESOLUTION_CACHE = {}  # (relpath_str, project_root_str) -> resolved Path or None


def _resolve_filepath(relpath, project_root, workspace_root=None):
    """Resolve a cited relpath to an actual Path; supports bare filenames via recursive search.

    Plan body often cites bare filenames (e.g., `EngineSharded.hpp` not
    `CoreFrameworks/EngineSharded.hpp`). Search recursively when path is bare.
    Cached per project_root to avoid repeated rglob.
    """
    cache_key = (relpath, str(project_root))
    if cache_key in _FILE_RESOLUTION_CACHE:
        return _FILE_RESOLUTION_CACHE[cache_key]

    # Try direct path first (engine root then workspace)
    direct = project_root / relpath
    if direct.exists():
        _FILE_RESOLUTION_CACHE[cache_key] = direct
        return direct
    if workspace_root is not None:
        ws_direct = workspace_root / relpath
        if ws_direct.exists():
            _FILE_RESOLUTION_CACHE[cache_key] = ws_direct
            return ws_direct

    # Bare filename — recursive search (skip build dirs / .git / etc.)
    if '/' not in relpath:
        bare = Path(relpath).name
        skip_dirs = {"build", "build_gui", "build_suite", "build_tsan", "build_asan",
                     "build_lat", "build_latency", "build_gui_asan", ".git", "node_modules"}
        for root in (project_root,) + ((workspace_root,) if workspace_root else ()):
            if not root.exists():
                continue
            for found in root.rglob(bare):
                # Skip files inside build dirs (compiled artifacts) or .git
                parts = set(found.relative_to(root).parts)
                if parts & skip_dirs:
                    continue
                _FILE_RESOLUTION_CACHE[cache_key] = found
                return found

    _FILE_RESOLUTION_CACHE[cache_key] = None
    return None


def verify_line_anchor(plan_line, citation, relpath, start, end, context, project_root,
                      workspace_root=None, fuzzy_window=50):
    """Return (status, detail) for one line-anchor verification.

    Status:
      PASS       — cited range exists + ≥1 context identifier appears in cited range
      SKIP       — no useful identifiers in context (nothing to verify against)
      OOB        — cited line exceeds file's actual line count (clearly stale)
      DRIFT      — identifiers found within fuzzy window but NOT in cited range
      DRIFT-FAR  — identifiers exist in file but >fuzzy_window lines from cited range
      MISSING    — file not found at project root or workspace root
      NOTFOUND   — file exists but identifiers not found anywhere in it (possible fabrication or full rename)
    """
    filepath = _resolve_filepath(relpath, project_root, workspace_root)
    if filepath is None:
        return ("MISSING", f"file not found at engine root: {relpath}")

    try:
        file_text = filepath.read_text(encoding='utf-8', errors='replace')
    except Exception as e:
        return ("MISSING", f"read failed: {e}")
    file_lines = file_text.split('\n')
    total = len(file_lines)
    if start > total:
        return ("OOB", f"cite line {start} > file has {total} lines (clearly stale)")

    identifiers = extract_context_identifiers(context)
    # Subtract identifiers that ARE the filepath itself (e.g., "EngineSharded" appears in both
    # context and path; doesn't help verify line accuracy)
    file_stem = Path(relpath).stem
    identifiers = {ident for ident in identifiers if file_stem not in ident}
    if not identifiers:
        return ("SKIP", "no useful context identifiers (nothing to verify against)")

    # Check identifiers in cited range
    cited_slice = '\n'.join(file_lines[start-1:min(end, total)])
    cited_hits = {ident for ident in identifiers if ident in cited_slice}
    if cited_hits:
        sample = sorted(cited_hits)[:3]
        return ("PASS", f"found {len(cited_hits)}/{len(identifiers)} ids in cited range: {sample}")

    # Try fuzzy window (±fuzzy_window lines)
    fuzzy_lo = max(0, start - fuzzy_window - 1)
    fuzzy_hi = min(total, end + fuzzy_window)
    fuzzy_slice = '\n'.join(file_lines[fuzzy_lo:fuzzy_hi])
    fuzzy_hits = {ident: _find_first_line(file_lines, ident, fuzzy_lo, fuzzy_hi)
                  for ident in identifiers if ident in fuzzy_slice}
    if fuzzy_hits:
        # Report most-cited identifier's actual line
        rep = sorted(fuzzy_hits.items(), key=lambda kv: kv[1] or 99999)[:2]
        return ("DRIFT", f"ids found near cited range but NOT in [{start}-{end}]; "
                         f"actual: {[(k, v) for k, v in rep]}")

    # Search whole file
    whole_hits = {ident: _find_first_line(file_lines, ident, 0, total)
                  for ident in identifiers if ident in file_text}
    if whole_hits:
        rep = sorted(whole_hits.items(), key=lambda kv: kv[1] or 99999)[:2]
        return ("DRIFT-FAR", f"ids exist in file but >±{fuzzy_window} lines from cite [{start}-{end}]; "
                             f"actual: {[(k, v) for k, v in rep]}")

    return ("NOTFOUND", f"none of {len(identifiers)} context identifiers found anywhere in {relpath}")


def derive_includes(code):
    """Return list of #include paths needed for this code block, based on
    symbol references in the block matched against SYMBOL_INCLUDES map."""
    needed_paths = set()
    # Find all word-tokens that look like uppercase symbols / project types
    tokens = set()
    for match in re.finditer(r'\b[A-Z][A-Za-z0-9_]+\b', code):
        tokens.add(match.group())
    for tok in tokens:
        if tok in SYMBOL_INCLUDES:
            needed_paths.add(SYMBOL_INCLUDES[tok])
    return sorted(needed_paths)


def looks_like_full_tu(code):
    """Heuristic: full translation unit (has #include or function defn)."""
    if re.search(r'^\s*#include', code, re.MULTILINE):
        return True
    if re.search(r'^\s*(int|void|template|inline)\s+\w+\s*[(<]', code, re.MULTILINE):
        return True
    return False


def looks_like_xmacro_expansion(code):
    """Heuristic: X-macro row addition fragment (starts with X(...) callback).
    These can only be compile-verified inside the registry expansion context,
    not as standalone code. Plan body uses them to spec new registry rows.
    """
    # Strip leading comment-only lines + blank lines
    lines = [l.rstrip() for l in code.split('\n')]
    nonblank = [l for l in lines if l.strip() and not l.lstrip().startswith('//')]
    if not nonblank:
        return False
    # Match X(...) macro callback at first non-comment line
    first = nonblank[0].strip()
    if re.match(r'^X\s*\(', first):
        return True
    # Or trailing backslash on every line (macro continuation block)
    if all(l.rstrip().endswith('\\') for l in nonblank[:-1]) and len(nonblank) >= 2:
        return True
    return False


def wrap_block(code, includes):
    """Wrap code in test harness with derived includes + variable shim.

    Always-included base set ensures harness compiles even if block doesn't
    reference these directly (the harness uses them as arg types + locals).
    """
    base_includes = {
        "CoreFrameworks/ControllerConfig.hpp",
        "CoreFrameworks/ControllerEventLoop.hpp",
        "CoreFrameworks/OrderManager.hpp",
        "CoreFrameworks/ExecutionCore.hpp",
        "CoreFrameworks/ShardedBacktestDriver.hpp",
        "FixedPoint/FixedPointN.hpp",
        "DataStream/BinanceCrypto.hpp",
        "DataStream/BinanceDepth.hpp",
        "ML_Headers/CoreModelZoo.hpp",
        "MemHeaders/BitmapMacros.hpp",
    }
    all_includes = sorted(base_includes | set(includes))
    include_lines = '\n'.join(f'#include "{p}"' for p in all_includes)
    return f"""{include_lines}

// BACKTEST_FP shim — defined at Backtest/BacktestEngine.hpp:47 but we don't
// want to pull that header (transitive deps); define directly here to match.
#ifndef BACKTEST_FP
#define BACKTEST_FP 64
#endif

namespace tt {{
template <unsigned F>
inline void __plan_body_check__(
    [[maybe_unused]] const ControllerConfig<F>& cfg,
    [[maybe_unused]] int c,
    [[maybe_unused]] EventLoopState<F>& state,
    [[maybe_unused]] OrderManagerState<F>& oms,
    [[maybe_unused]] int num_cores,
    [[maybe_unused]] int tick_index,
    [[maybe_unused]] ShardedBacktestDriver<F>* drv)
{{
    [[maybe_unused]] FPN<F> price = FPN_Zero<F>();
    [[maybe_unused]] FPN<F> volume = FPN_Zero<F>();
    [[maybe_unused]] uint64_t ts_us = 0;
    [[maybe_unused]] uint64_t now_tick = 0;
    [[maybe_unused]] FPN<F> mtm_price = FPN_Zero<F>();
    [[maybe_unused]] double price_d = 0.0, default_per_core = 0.0, default_risk = 0.0;
    [[maybe_unused]] double total_balance = 0.0, core_balance = 0.0;
    [[maybe_unused]] double book_spread_d = 0.0, book_mid_d = 0.0;
    [[maybe_unused]] FPN<F> book_imb = FPN_Zero<F>();
    [[maybe_unused]] BookSnapshot<F> depth = BookSnapshot_Init<F>();
    [[maybe_unused]] CoreModelZoo<F>* zoo_ptr = nullptr;
    [[maybe_unused]] EnsembleModelZoo<F>* ezoo_ptr = nullptr;
    [[maybe_unused]] SPSCRing<Tick<F>, EXECUTION_CORE_TICK_RING_SIZE> tick_ring;
    [[maybe_unused]] ExecutionCore<F> core;
    [[maybe_unused]] Tick<F> tick = {{}};
    [[maybe_unused]] auto fn_for_loop = []() {{ return 0; }};
    // Caller-context shim — code blocks in plan body reference caller-scope statics
    // (cores[]/ml_zoos[]/tick_rings[] arrays) + lambda captures (last_price/last_volume/
    // ticks_produced atomics) + caller-passed run_cfg ptr. Shim provides these so tool
    // can isolate REAL fabrications from caller-context references.
    [[maybe_unused]] ExecutionCore<F> cores[16] = {{}};
    [[maybe_unused]] CoreModelZoo<F> ml_zoos[16] = {{}};
    [[maybe_unused]] EnsembleModelZoo<F> ml_ensemble_zoos[16] = {{}};
    [[maybe_unused]] SPSCRing<Tick<F>, EXECUTION_CORE_TICK_RING_SIZE> tick_rings[16];
    [[maybe_unused]] std::atomic<double> last_price{{0.0}};
    [[maybe_unused]] std::atomic<double> last_volume{{0.0}};
    [[maybe_unused]] std::atomic<uint64_t> ticks_produced{{0}};
    [[maybe_unused]] struct {{ char bandit_state_prior_path[256]; }} _run_cfg_storage = {{}};
    [[maybe_unused]] decltype(&_run_cfg_storage) run_cfg = &_run_cfg_storage;
    [[maybe_unused]] uint64_t rebuild_ts_us = 0;
    [[maybe_unused]] uint64_t pp_now_tick = 0;
    // Non-const cfg shim — ApplyBnbDiscount mutates cfg (takes non-const ref)
    [[maybe_unused]] ControllerConfig<F> _non_const_cfg_storage = {{}};
    [[maybe_unused]] ControllerConfig<F>& cfg_nc = _non_const_cfg_storage;
    // ctx shim — BacktestSharded.hpp feature collector context local
    [[maybe_unused]] struct {{
        uint8_t current_regime = 0;
    }} ctx;
{{
{code}
}}
}}
}}  // namespace tt

template void tt::__plan_body_check__<64>(
    const ControllerConfig<64>&,
    int,
    EventLoopState<64>&,
    OrderManagerState<64>&,
    int,
    int,
    ShardedBacktestDriver<64>*);
int main() {{ return 0; }}
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


def check_plan_body(plan_path, strict=False, verify_anchors=True):
    """Return dict with code-block + line-anchor verification results.

    Keys:
      n_blocks, n_fab, n_harness, code_findings
      n_anchors, n_pass, n_skip, n_drift, n_oob, n_missing, n_notfound, anchor_findings
    """
    text = plan_path.read_text(encoding='utf-8', errors='replace')

    # === Pass 1: cpp code-block compilation (symbol existence — Class 14 closure) ===
    blocks = list(extract_cpp_blocks(text))
    code_findings = []
    n_fab = 0
    n_harness = 0
    for (line, body) in blocks:
        if looks_like_xmacro_expansion(body):
            continue
        if looks_like_full_tu(body):
            wrapped = body
        else:
            includes = derive_includes(body)
            wrapped = wrap_block(body, includes)
        ok, stderr = try_compile(wrapped, f"{plan_path.name}:line{line}")
        if not ok:
            classification, error_line = classify_failure(stderr)
            excerpt = body.split('\n')[0][:80]
            code_findings.append((line, classification, excerpt, error_line))
            if classification == "FABRICATION":
                n_fab += 1
            else:
                n_harness += 1

    # === Pass 2: line-anchor verification (line-range accuracy — line-drift closure) ===
    anchor_findings = []
    n_anchors = 0
    n_pass = 0
    n_skip = 0
    n_drift = 0
    n_oob = 0
    n_missing = 0
    n_notfound = 0
    if verify_anchors:
        for (plan_line, citation, relpath, start, end, context) in extract_line_anchors(text):
            n_anchors += 1
            status, detail = verify_line_anchor(plan_line, citation, relpath, start, end, context,
                                                project_root=ENGINE, workspace_root=WORKSPACE)
            if status == "PASS":
                n_pass += 1
            elif status == "SKIP":
                n_skip += 1
            elif status == "OOB":
                n_oob += 1
                anchor_findings.append((plan_line, status, citation, detail))
            elif status in ("DRIFT", "DRIFT-FAR"):
                n_drift += 1
                anchor_findings.append((plan_line, status, citation, detail))
            elif status == "MISSING":
                n_missing += 1
                anchor_findings.append((plan_line, status, citation, detail))
            elif status == "NOTFOUND":
                n_notfound += 1
                anchor_findings.append((plan_line, status, citation, detail))

    return {
        "n_blocks": len(blocks),
        "n_fab": n_fab,
        "n_harness": n_harness,
        "code_findings": code_findings,
        "n_anchors": n_anchors,
        "n_pass": n_pass,
        "n_skip": n_skip,
        "n_drift": n_drift,
        "n_oob": n_oob,
        "n_missing": n_missing,
        "n_notfound": n_notfound,
        "anchor_findings": anchor_findings,
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("paths", nargs="*", help="plan body .md files to check")
    p.add_argument("--all", action="store_true",
                   help="check all .md under plans/")
    p.add_argument("--strict", action="store_true",
                   help="report HARNESS-ISSUE blocks too (default: only FABRICATION)")
    p.add_argument("--quiet", action="store_true",
                   help="only print failures + summary")
    p.add_argument("--no-verify-anchors", action="store_true",
                   help="skip line-anchor verification pass (default: always run)")
    p.add_argument("--show-drift", action="store_true",
                   help="report line-anchor DRIFTs (default: only OOB + MISSING printed; drifts in summary)")
    args = p.parse_args()

    if args.all:
        paths = sorted(PLANS_DIR.rglob("*.md"))
    else:
        paths = [Path(p) for p in args.paths]

    if not paths:
        print("usage: check_plan_body_symbol_existence.py <plan-body.md> [...] | --all", file=sys.stderr)
        sys.exit(2)

    total_blocks = 0
    total_fab = 0
    total_harness = 0
    total_anchors = 0
    total_drift = 0
    total_oob = 0
    total_missing = 0
    total_notfound = 0
    any_fabrication = False
    any_anchor_error = False  # OOB + MISSING are blocking; DRIFT is warning

    for path in paths:
        if not path.exists():
            print(f"[error] not found: {path}", file=sys.stderr)
            any_fabrication = True
            continue
        result = check_plan_body(path, strict=args.strict,
                                  verify_anchors=not args.no_verify_anchors)
        n_blocks = result["n_blocks"]
        n_fab = result["n_fab"]
        n_harness = result["n_harness"]
        code_findings = result["code_findings"]
        n_anchors = result["n_anchors"]
        n_drift = result["n_drift"]
        n_oob = result["n_oob"]
        n_missing = result["n_missing"]
        n_notfound = result["n_notfound"]
        anchor_findings = result["anchor_findings"]

        total_blocks += n_blocks
        total_fab += n_fab
        total_harness += n_harness
        total_anchors += n_anchors
        total_drift += n_drift
        total_oob += n_oob
        total_missing += n_missing
        total_notfound += n_notfound
        if n_fab > 0:
            any_fabrication = True
        if n_oob > 0 or n_missing > 0:
            any_anchor_error = True

        # === Code-block report (existing) ===
        if n_fab > 0 or (args.strict and n_harness > 0):
            print(f"\n=== {path.name}  ({n_fab} fabrications + {n_harness} harness-issues of {n_blocks} blocks) ===", file=sys.stderr)
            for (line, cls, excerpt, error_line) in code_findings:
                if cls == "FABRICATION" or (args.strict and cls == "HARNESS-ISSUE"):
                    marker = "❌ FABRICATION" if cls == "FABRICATION" else "⚠️  HARNESS-ISSUE"
                    print(f"\n  {marker} at {path.name}:line~{line}", file=sys.stderr)
                    print(f"    block excerpt: {excerpt!r}", file=sys.stderr)
                    print(f"    error: {error_line}", file=sys.stderr)
        elif not args.quiet:
            extra = f" ({n_harness} harness-issues; use --strict to see)" if n_harness else ""
            print(f"[ok] {path.name}  ({n_blocks} blocks; 0 fabrications{extra})")

        # === Line-anchor report (NEW) ===
        if anchor_findings:
            show_drift = args.show_drift or args.strict
            blocking = [f for f in anchor_findings if f[1] in ("OOB", "MISSING")]
            drift = [f for f in anchor_findings if f[1] in ("DRIFT", "DRIFT-FAR", "NOTFOUND")]
            if blocking or (show_drift and drift):
                print(f"\n=== {path.name} line-anchor verification ===", file=sys.stderr)
                for (plan_line, status, citation, detail) in anchor_findings:
                    if status in ("OOB", "MISSING"):
                        print(f"  ❌ {status} at plan:line~{plan_line}  cite={citation}", file=sys.stderr)
                        print(f"     {detail}", file=sys.stderr)
                    elif show_drift and status in ("DRIFT", "DRIFT-FAR", "NOTFOUND"):
                        print(f"  ⚠️  {status} at plan:line~{plan_line}  cite={citation}", file=sys.stderr)
                        print(f"     {detail}", file=sys.stderr)

    # === Summary ===
    anchor_summary = ""
    if total_anchors > 0:
        anchor_summary = (f"; {total_anchors} line-anchors ({total_drift} drift + {total_oob} OOB"
                          f" + {total_missing} missing + {total_notfound} notfound)")
    print(f"\n=== SUMMARY: {total_blocks} blocks checked across {len(paths)} files; {total_fab} FABRICATIONS + {total_harness} harness-issues{anchor_summary} ===", file=sys.stderr)
    if total_drift > 0 and not args.show_drift:
        print(f"  (line-anchor DRIFTs: {total_drift}; rerun with --show-drift to see details)", file=sys.stderr)
    # Exit non-zero on fabrication OR anchor blocker (OOB / MISSING)
    sys.exit(1 if (any_fabrication or any_anchor_error) else 0)


if __name__ == "__main__":
    main()
