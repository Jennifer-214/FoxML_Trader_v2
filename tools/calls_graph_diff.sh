#!/bin/bash
# tools/calls_graph_diff.sh — find functions called only in legacy entrypoint
# but not in any sharded entrypoint. Catches orphaned-after-architectural-
# migration patterns like the v4.x sharded port that silently dropped
# Strategy_Adapt / _BuySignal / _ExitAdjust calls (root cause of v5.4
# postmortem F7-F10).
#
# Usage:
#   ./tools/calls_graph_diff.sh
#
# Exits 0 with empty output = clean (no orphans). Non-empty output =
# functions to review (likely orphaned, possibly intentionally skipped).
#
# This is the tool the readiness skill (Phase 6.1) will require for any
# plan that touches strategy code or per-core architecture.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

LEGACY_FILE="CoreFrameworks/PortfolioController.hpp"
SHARDED_FILES=(
    "CoreFrameworks/ControllerEventLoop.hpp"
    "CoreFrameworks/EngineSharded.hpp"
    "CoreFrameworks/ShardedBacktestDriver.hpp"
    "Strategies/StrategyParameters.hpp"
    "Strategies/StrategyLifecycle.hpp"  # v5.4.0+ per-core dispatch
)

# Extract function calls (Pattern_FunctionName followed by '(') excluding
# comment lines + definitions. We want USAGE only.
extract_calls() {
    local file=$1
    grep -hE '[A-Z][A-Za-z0-9]+_[A-Z][A-Za-z0-9]+\s*\(' "$file" 2>/dev/null \
        | grep -vE '^\s*//' \
        | grep -vE '^\s*\*' \
        | grep -oE '[A-Z][A-Za-z0-9]+_[A-Z][A-Za-z0-9]+' \
        | sort -u
}

# Strategy/regime-related functions only; full diff would be too noisy.
# Add module patterns here as new families need orphan-checking.
MODULE_PATTERNS=(
    'Momentum_'
    'MeanReversion_'
    'SimpleDip_'
    'EmaCross_'
    'MLStrategy_'
    'Regime_'
    'Strategy_'
)

# Compose grep filter
PATTERN_RE=$(printf '|%s' "${MODULE_PATTERNS[@]}")
PATTERN_RE="^(${PATTERN_RE:1})"

# Calls in legacy entrypoint
LEGACY_CALLS=$(extract_calls "$LEGACY_FILE" | grep -E "$PATTERN_RE" || true)

# Calls in sharded entrypoints (union)
SHARDED_CALLS=$(
    for f in "${SHARDED_FILES[@]}"; do
        extract_calls "$f"
    done | grep -E "$PATTERN_RE" | sort -u
)

# Diff: in legacy but not in sharded
ORPHAN_CANDIDATES_RAW=$(comm -23 \
    <(echo "$LEGACY_CALLS" | sort -u) \
    <(echo "$SHARDED_CALLS" | sort -u))

# Subtract whitelisted baseline (known-expected legacy orphans).
# See tools/calls_graph_diff_baseline.txt for the policy + entries.
# Baseline file uses one symbol per line; # comments + blank lines ignored.
BASELINE_FILE="$REPO_ROOT/tools/calls_graph_diff_baseline.txt"
if [[ -f "$BASELINE_FILE" ]]; then
    BASELINE=$(grep -vE '^\s*(#|$)' "$BASELINE_FILE" | sort -u)
    ORPHAN_CANDIDATES=$(comm -23 \
        <(echo "$ORPHAN_CANDIDATES_RAW" | sort -u) \
        <(echo "$BASELINE"))
else
    ORPHAN_CANDIDATES="$ORPHAN_CANDIDATES_RAW"
fi

# Also detect: functions defined in Strategies/ but called nowhere at all.
# Different bug class from "orphaned in migration" — these are defined and
# forgotten entirely. The dust skill's orphan-detection scan covers this
# more broadly; here we just check the strategy/regime families.
DEFINED_FNS=$(
    grep -hE '^(inline|template|static).*[A-Z][A-Za-z0-9]+_[A-Z][A-Za-z0-9]+\s*\(' \
        /home/caramel/code/tick-trader-percore/Strategies/*.hpp \
        /home/caramel/code/tick-trader-percore/Strategies/private/*.hpp \
        2>/dev/null \
    | grep -oE '[A-Z][A-Za-z0-9]+_[A-Z][A-Za-z0-9]+' \
    | grep -E "$PATTERN_RE" \
    | sort -u
)
ALL_CALLS=$(
    {
        extract_calls "$LEGACY_FILE"
        for f in "${SHARDED_FILES[@]}"; do
            extract_calls "$f"
        done
    } | grep -E "$PATTERN_RE" | sort -u
)
NEVER_CALLED_RAW=$(comm -23 \
    <(echo "$DEFINED_FNS") \
    <(echo "$ALL_CALLS"))

# Same baseline subtraction for dead-defined: legacy variants kept for tests.
if [[ -f "$BASELINE_FILE" ]]; then
    NEVER_CALLED=$(comm -23 \
        <(echo "$NEVER_CALLED_RAW" | sort -u) \
        <(echo "$BASELINE"))
else
    NEVER_CALLED="$NEVER_CALLED_RAW"
fi

if [[ -z "$ORPHAN_CANDIDATES" && -z "$NEVER_CALLED" ]]; then
    echo "[calls-graph-diff] CLEAN — no strategy/regime functions orphaned or dead-defined"
    exit 0
fi

if [[ -n "$NEVER_CALLED" ]]; then
    echo "[calls-graph-diff] DEAD-DEFINED — $(echo "$NEVER_CALLED" | wc -l) function(s) defined but never called:"
    echo "$NEVER_CALLED" | sed 's/^/  /'
    echo
fi

if [[ -z "$ORPHAN_CANDIDATES" ]]; then
    if [[ -n "$NEVER_CALLED" ]]; then
        echo "[calls-graph-diff] No migration-orphans, but dead-defined functions present (above)."
        echo "Exit code 1 — review and either wire in or delete."
        exit 1
    fi
    exit 0
fi

echo "[calls-graph-diff] FOUND $(echo "$ORPHAN_CANDIDATES" | wc -l) orphan candidate(s)"
echo "[calls-graph-diff] These are called in $LEGACY_FILE but not in any sharded entrypoint:"
echo "$ORPHAN_CANDIDATES" | sed 's/^/  /'
echo
echo "[calls-graph-diff] Review each:"
echo "  - WIRE_IN: strategy lifecycle stage missing in sharded — restore it"
echo "  - SKIP_INTENTIONAL: documented in DOCS/STRATEGY_INTERFACE.md compliance matrix"
echo "  - REMOVE: function is genuinely obsolete — delete from Strategies/*.hpp"
echo
echo "Exit code 1 — orphans present. See DOCS/v5.4-regression-postmortem.md"
echo "for the historical incident this tool was built to prevent."
exit 1
