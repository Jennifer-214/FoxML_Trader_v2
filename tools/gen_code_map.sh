#!/bin/bash
# gen_code_map.sh — generate DOCS/CODE_MAP.md from .hpp file scan.
#
# Strategy: grep for function definitions matching the codebase's
# Pattern_FunctionName convention. For each match, capture the
# preceding non-empty comment line as the one-line purpose.
#
# Usage: ./tools/gen_code_map.sh [output_path]
#   default output: DOCS/CODE_MAP.md
#
# Re-run whenever you want a fresh index. Cheap (< 5 sec on this codebase).

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO_ROOT/DOCS/CODE_MAP.md}"

cd "$REPO_ROOT"

# Subsystems we map. Order matters for the output.
SUBSYSTEMS=(
    "CoreFrameworks"
    "Strategies"
    "Strategies/private"
    "DataStream"
    "FixedPoint"
    "MemHeaders"
    "ML_Headers"
    "GUI"
    "Backtest"
    "tests"
)

# One-line purpose extraction.
#
# The codebase uses banner-style comment blocks above functions:
#
#     //=====================================================================
#     // [SECTION NAME]
#     //=====================================================================
#     // Description line 1
#     // Description line 2
#     //=====================================================================
#     inline void Foo_Bar(...)
#
# We want the first descriptive line — skip banner dividers (=== / ---),
# skip [SECTION] markers (these are usually the function name), and
# capture the first real `// text` line working backward from the
# function def.
extract_purpose() {
    local file="$1"
    local lineno="$2"
    # Look at lines (target-15 .. target-1), reversed, find first plain
    # description line.
    local start=$((lineno - 15))
    if [ "$start" -lt 1 ]; then start=1; fi
    sed -n "${start},$((lineno-1))p" "$file" | awk '
        # Strip comment prefix
        /^[[:space:]]*\/\// {
            line = $0
            sub(/^[[:space:]]*\/\/[[:space:]]*/, "", line)
            # Skip banner dividers
            if (line ~ /^[=-]+$/) next
            # Skip [SECTION] markers (often the function name in caps)
            if (line ~ /^\[.*\]$/) next
            # Skip empty after strip
            if (line == "") next
            # Capture this — but keep going to find the LAST descriptive
            # line (closest to the function def)
            last_desc = line
            next
        }
        # Non-comment line resets the buffer (we hit code above)
        /^[[:space:]]*[a-zA-Z]/ {
            last_desc = ""
        }
        END {
            # Trim to a sensible length
            if (length(last_desc) > 120) last_desc = substr(last_desc, 1, 117) "..."
            print last_desc
        }
    '
}

# Function-def regex:
# - `inline ... Pattern_FunctionName(`
# - `static inline ... Pattern_FunctionName(`
# - `template ... \n inline ... Pattern_FunctionName(` (multi-line — we only
#   catch the line with the open paren, the template line preceding is fine
#   since we don't render it)
#
# Pattern_FunctionName = at least one capital letter + underscore + identifier
# (avoids matching ALLCAPS_MACROS and lowercase_helpers).
FN_REGEX='^[[:space:]]*((static[[:space:]]+)?inline[[:space:]]+)([A-Za-z_<>:&* ]+[[:space:]])([A-Z][A-Za-z0-9]*_[A-Za-z0-9_]+)[[:space:]]*\('

# Output header
{
    echo "# CODE_MAP.md"
    echo
    echo "Auto-generated function index. Walks .hpp files in each subsystem and extracts \`Pattern_FunctionName\` style definitions with their one-line purpose (from the preceding \`//\` comment, when present)."
    echo
    echo "**Re-generate**: \`./tools/gen_code_map.sh\`"
    echo
    echo "**Last regenerated**: $(date +%Y-%m-%d) (commit $(git rev-parse --short HEAD 2>/dev/null || echo unknown))"
    echo

    for subsys in "${SUBSYSTEMS[@]}"; do
        if [ ! -d "$subsys" ]; then
            continue
        fi

        # Count files first; skip empty subsystems
        file_count=$(find "$subsys" -maxdepth 1 -name "*.hpp" -o -name "*.cpp" 2>/dev/null | wc -l)
        if [ "$file_count" -eq 0 ]; then
            continue
        fi

        echo "## $subsys/"
        echo

        # Walk each file at depth 1 only (subdirs handled separately, e.g. Strategies/private/)
        for file in $(find "$subsys" -maxdepth 1 -name "*.hpp" -o -name "*.cpp" 2>/dev/null | sort); do
            base=$(basename "$file")

            # Find function defs in this file
            matches=$(grep -nE "$FN_REGEX" "$file" 2>/dev/null || true)
            if [ -z "$matches" ]; then
                continue
            fi

            echo "### $base"
            echo

            # For each match, extract func name + line number + purpose
            while IFS= read -r match; do
                lineno=$(echo "$match" | cut -d: -f1)
                # Extract Pattern_FunctionName from the line
                fname=$(echo "$match" | sed -nE "s/.*[[:space:]]([A-Z][A-Za-z0-9]*_[A-Za-z0-9_]+)[[:space:]]*\(.*/\1/p")
                if [ -z "$fname" ]; then
                    continue
                fi
                # Skip duplicates (same function name appears twice in some headers due to overloads)
                purpose=$(extract_purpose "$file" "$lineno")
                if [ -n "$purpose" ]; then
                    echo "- \`${fname}\` — line ${lineno} — ${purpose}"
                else
                    echo "- \`${fname}\` — line ${lineno}"
                fi
            done <<< "$matches"

            echo
        done

    done

    echo "---"
    echo
    echo "## Top-level files"
    echo
    for f in main.cpp Version.hpp Limits.hpp; do
        if [ -f "$f" ]; then
            line_count=$(wc -l < "$f")
            echo "- \`$f\` — $line_count lines"
        fi
    done
    echo

    echo "## Conventions"
    echo
    echo "- Function names follow \`Pattern_FunctionName\` convention (e.g. \`Portfolio_Init\`, \`BG_Evaluate\`)"
    echo "- Headers are inline-heavy — most functions live in \`.hpp\` and are \`inline\`"
    echo "- Templates parameterize on \`unsigned F\` (FPN word count), default \`F=64\` (4096-bit)"
    echo "- Lowercase helpers (\`fan_out\`, \`drain_with_submit\`) are local to a function and not in this map"
    echo "- ALL_CAPS macros are not in this map; see headers directly"

} > "$OUT"

# Final stats
fn_count=$(grep -c "^- \`" "$OUT" || echo 0)
echo "[gen_code_map] wrote $OUT — $fn_count functions indexed across ${#SUBSYSTEMS[@]} subsystems"
