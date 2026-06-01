#!/bin/bash
# gen_code_map.sh — code-intelligence index: function map + type->sites blast-radius.
#
# Strategy: grep for function definitions matching the codebase's
# Pattern_FunctionName convention. For each match, capture the
# preceding non-empty comment line as the one-line purpose.
# The --types/--structs/--full modes (folded from gen_type_map, D-134) add a
# type->sites reverse-index for "what is the blast radius of changing type T?".
#
# Usage:
#   ./tools/gen_code_map.sh [output_path]    # regenerate DOCS/CODE_MAP.md (function index; default)
#   ./tools/gen_code_map.sh --types   <T>    # classify every T<...> ref: DECL/RETURN/PARAM/ALIAS/OTHER
#   ./tools/gen_code_map.sh --structs <T>    # structs/classes embedding T as a field (byte-layout blast set)
#   ./tools/gen_code_map.sh --full    <T>    # --structs + --types in tandem
#
# Re-run whenever you want a fresh index. Cheap (< 5 sec on this codebase).

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

#======================================================================================================
# [TYPE->SITES MODES — folded in from the former gen_type_map.sh (decision D-134)]
#======================================================================================================
# --types <T>    classify every `T<...>` reference: DECL / RETURN / PARAM / ALIAS / OTHER
# --structs <T>  structs/classes EMBEDDING T as a field — the byte-layout blast set (memcmp/SHA/fwrite)
# --full <T>     --structs then --types, in tandem (granular control + detailed combined output)
# Grep/rg-based (distinctive tokens like FPN trace cleanly; NOT full AST). The DECL bucket still mixes
# struct-field + local; --structs is the scope-aware answer to "which structs". Per D-134.
type_map() {
    local TYPE="$1"
    rg -n -g '*.hpp' -g '*.cpp' "${TYPE}<" 2>/dev/null | awk -v TYPE="$TYPE" '
    function classify(t,   reT) {
        reT = TYPE "<[^>]*>"
        gsub(/^[ \t]+/, "", t)
        if (t ~ /^(\/\/|\*|\/\*)/)                                                    return "OTHER"
        if (t ~ ("(using|typedef)[^;]*" TYPE "<"))                                    return "ALIAS"
        if (t ~ (reT "[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*\\("))                        return "RETURN"
        if (t ~ (reT "[ \t]+[A-Za-z_][A-Za-z0-9_]*([ \t]*\\[[^]]*\\])?[ \t]*[;=,]"))  return "DECL"
        if (t ~ /(sizeof|^(for|while|if|switch))[ \t]*\(/)                            return "OTHER"
        if (t ~ ("[A-Za-z_][A-Za-z0-9_]*[ \t]*\\([^)]*" reT))                         return "PARAM"
        return "OTHER"
    }
    { i1 = index($0, ":"); rest = substr($0, i1 + 1); i2 = index(rest, ":")
      file = substr($0, 1, i1 - 1); ln = substr(rest, 1, i2 - 1); text = substr(rest, i2 + 1)
      c = classify(text); cnt[c]++; total++
      if (n[c] < 8) { site[c, n[c]] = file ":" ln "  " text; n[c]++ } }
    END {
      split("DECL RETURN PARAM ALIAS OTHER", ord, " ")
      printf "# Type usage: %s  (%d refs across .hpp/.cpp)\n\n", TYPE, total
      for (oi = 1; oi <= 5; oi++) { c = ord[oi]; printf "## %s (%d)\n", c, cnt[c] + 0
        for (j = 0; j < n[c]; j++) { s = site[c, j]; if (length(s) > 140) s = substr(s, 1, 137) "..."; print "  " s }
        if (cnt[c] + 0 > n[c]) printf "  ... (+%d more)\n", cnt[c] - n[c]; print "" } }
    '
}

struct_map() {
    local TYPE="$1"
    local files; files=$(rg -l -g '*.hpp' -g '*.cpp' "${TYPE}<" 2>/dev/null || true)
    [ -z "$files" ] && { echo "# no .hpp/.cpp references ${TYPE}<"; return 0; }
    awk -v TYPE="$TYPE" '
    FNR == 1 { sp = 0; pending = ""; depth = 0 }
    {
      if (match($0, /(struct|class)[ \t]+(alignas[ \t]*\([^)]*\)[ \t]*)?[A-Za-z_][A-Za-z0-9_]*/)) {
        nm = substr($0, RSTART, RLENGTH); sub(/^(struct|class)[ \t]+/, "", nm); sub(/^alignas[ \t]*\([^)]*\)[ \t]*/, "", nm); pending = nm }
      if (pending != "" && $0 ~ /;/ && $0 !~ /{/) pending = ""
      o = $0; no = gsub(/{/, "", o); c2 = $0; nc = gsub(/}/, "", c2)
      if (no > 0 && pending != "") { sp++; sname[sp] = pending; sdepth[sp] = depth + 1; pending = "" }
      depth += no - nc
      while (sp > 0 && depth < sdepth[sp]) sp--
      if (sp > 0 && depth == sdepth[sp]) {
        reT = TYPE "<[^>]*>"
        if ($0 ~ (reT "[ \t]+[A-Za-z_][A-Za-z0-9_]*([ \t]*\\[[^]]*\\])?[ \t]*;")) {
          fld = "?"
          if (match($0, (reT "[ \t]+[A-Za-z_][A-Za-z0-9_]*"))) { fld = substr($0, RSTART, RLENGTH); sub((reT "[ \t]+"), "", fld) }
          key = FILENAME "|" sname[sp]
          if (!(key in seen)) { seen[key] = 1; ordr[++on] = key; sf[key] = sname[sp]; ff[key] = FILENAME }
          flds[key] = flds[key] " " fld } } }
    END {
      printf "# Structs/classes embedding %s as a field (byte-layout blast set)\n\n", TYPE
      if (on == 0) print "  (none at struct-body scope)"
      for (i = 1; i <= on; i++) { k = ordr[i]; printf "  %-34s struct %-26s {%s }\n", ff[k], sf[k], flds[k] } }
    ' $files
}

# --macros <T>: X-macro-defined fields of type T — the registry structs --structs MISSES
# (e.g. Position via PositionFieldRegistry `X(entry_price, T<...>, ...)` rows). Per the #11 0a finding;
# complete byte-layout set = --structs + --macros (or --full).
macro_map() {
    local TYPE="$1"
    rg -n -g '*.hpp' "X\([A-Za-z_][A-Za-z0-9_]*,[ \t]*${TYPE}<" 2>/dev/null | awk '
    { i1 = index($0, ":"); rest = substr($0, i1 + 1); i2 = index(rest, ":")
      file = substr($0, 1, i1 - 1); text = substr(rest, i2 + 1)
      fld = "?"; if (match(text, /X\([A-Za-z_][A-Za-z0-9_]*/)) { fld = substr(text, RSTART, RLENGTH); sub(/X\([ \t]*/, "", fld) }
      if (!(file in seen)) { seen[file] = 1; ordr[++on] = file }
      flds[file] = flds[file] " " fld }
    END {
      printf "# X-macro-defined %s fields (registry structs that --structs misses)\n\n", "'"$TYPE"'"
      if (on == 0) print "  (none)"
      for (i = 1; i <= on; i++) { f = ordr[i]; printf "  %-48s {%s }\n", f, flds[f] } }
    '
}

# --composition <T>: structs byte-affected by T via TRANSITIVE containment — a struct that contains
# a struct that contains T (however deep). --structs/--macros only catch DIRECT fields; this is the
# "every case" for byte-layout (the tail that hides the dangerous misses). Per the trust-100% mandate.
composition_map() {
    local TYPE="$1"
    awk -v TYPE="$TYPE" '
    FNR == 1 { sp = 0; pending = ""; depth = 0 }
    {
      if (match($0, /(struct|class)[ \t]+(alignas[ \t]*\([^)]*\)[ \t]*)?[A-Za-z_][A-Za-z0-9_]*/)) {
        nm = substr($0, RSTART, RLENGTH); sub(/^(struct|class)[ \t]+/, "", nm); sub(/^alignas[ \t]*\([^)]*\)[ \t]*/, "", nm); pending = nm }
      if (pending != "" && $0 ~ /;/ && $0 !~ /{/) pending = ""
      o = $0; no = gsub(/{/, "", o); c2 = $0; nc = gsub(/}/, "", c2)
      if (no > 0 && pending != "") { sp++; sname[sp] = pending; sdepth[sp] = depth + 1; pending = ""; structset[sname[sp]] = 1 }
      depth += no - nc
      while (sp > 0 && depth < sdepth[sp]) sp--
      if (sp > 0 && depth == sdepth[sp]) {
        line = $0; gsub(/^[ \t]+/, "", line)
        if (line ~ /^(static|constexpr|using|typedef|template|inline|\/\/|\*|#|public:|private:|protected:)/) next
        if (line ~ /\(/) next
        if (match(line, /^[A-Za-z_][A-Za-z0-9_:]*(<[^>;{}]*>)?[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*(\[[^]]*\])?[ \t]*[;=]/)) {
          tt = line; sub(/[<[ \t=].*/, "", tt)
          key = sname[sp] SUBSEP tt
          if (!(key in fseen)) { fseen[key] = 1; flds[sname[sp]] = flds[sname[sp]] " " tt } } }
    }
    END {
      for (s in structset) { n = split(flds[s], a, " "); for (i = 1; i <= n; i++) if (a[i] == TYPE) { aff[s] = 1; direct[s] = 1 } }
      changed = 1
      while (changed) { changed = 0
        for (s in structset) { if (s in aff) continue
          n = split(flds[s], a, " "); for (i = 1; i <= n; i++) if (a[i] in aff) { aff[s] = 1; changed = 1; break } } }
      printf "# Structs byte-affected by %s via TRANSITIVE composition (the cases --structs/--macros MISS)\n\n", TYPE
      nd = 0; for (s in direct) nd++
      cnt = 0
      for (s in aff) if (!(s in direct)) { if (length(s) < 3 || s ~ /^[a-z]/) continue; printf "  [transitive] %s\n", s; cnt++ }
      if (cnt == 0) print "  (none — no struct transitively contains " TYPE " beyond the direct set)"
      printf "\n  (%d DIRECT containers [--structs/--macros] + %d TRANSITIVE [composition-only])\n", nd, cnt }
    ' $(rg -l -g '*.hpp' -g '*.cpp' "(struct|class)[ \t]" 2>/dev/null)
}

# --byte-context <T>: the ENFORCEMENT-target sites — sizeof(T) + the memcmp/SHA/fwrite/HMAC ops where a
# static_assert(sizeof)/golden guard must live (discovery feeds enforcement; enforcement is the guarantee).
byte_context() {
    local TYPE="$1"
    echo "## sizeof(${TYPE}...) sites — size-dependent code (each wants a static_assert(sizeof==N))"
    rg -n -g '*.hpp' -g '*.cpp' "sizeof\([^)]*${TYPE}" 2>/dev/null | sed 's/^/  /' || echo "  (none)"
    echo ""
    echo "## byte-equivalence ops (memcmp/SHA256_Update/fwrite/fread/HMAC) — layout-locked surfaces to cross-ref vs the ${TYPE} structs"
    rg -n -g '*.hpp' -g '*.cpp' "(memcmp|SHA256_Update|HMAC|fwrite|fread)[ \t]*\(" 2>/dev/null | sed 's/^/  /' | head -50
}

# --aliases <T>: type aliases that resolve to T (using/typedef) — fields of these are ALSO T (the alias gap).
alias_map() {
    local TYPE="$1"
    echo "## aliases of ${TYPE} (additional --structs roots — a field of one of these is a ${TYPE} field):"
    rg -n -g '*.hpp' -g '*.cpp' "(using|typedef)[ \t][^;]*${TYPE}<" 2>/dev/null | sed 's/^/  /' || echo "  (none)"
}

# --callers <FN>: DIRECT textual call-sites. HONEST LIMIT: grep misses fn-pointer tables + X-macro dispatch
# (calls_graph_diff covers the X-macro part; a complete call-graph = clang, task #23). Never trust "0 callers".
callers_map() {
    local FN="$1"
    echo "## DIRECT call-sites of ${FN}(  — ⚠ grep-only: MISSES fn-pointer tables + X-macro dispatch (clang = #23); never read '0' as 'safe to change'"
    rg -n -g '*.hpp' -g '*.cpp' "\b${FN}[ \t]*\(" 2>/dev/null | rg -v ":[0-9]+:[ \t]*(//|\*)" | sed 's/^/  /' | head -60 || echo "  (none — but see the banner: verify fn-pointer/X-macro dispatch by hand)"
}

# clean missing-arg error (vs the bash-noisy ${2:?}) for the modes that need a TYPE/FN argument:
case "${1:-}" in
    --types|--structs|--composition|--byte-context|--aliases|--callers|--macros|--full)
        [ -n "${2:-}" ] || { echo "gen_code_map: $1 needs a TYPE/FN argument — e.g.  $1 FPN" >&2; exit 2; } ;;
esac

case "${1:-}" in
    --types)   type_map "${2:?--types needs a TYPE, e.g. FPN}";   exit 0 ;;
    --composition) composition_map "${2:?--composition needs a TYPE, e.g. FPN}"; exit 0 ;;
    --byte-context) byte_context "${2:?--byte-context needs a TYPE}"; exit 0 ;;
    --aliases) alias_map "${2:?--aliases needs a TYPE}"; exit 0 ;;
    --callers) callers_map "${2:?--callers needs a FN, e.g. FP64_Mul}"; exit 0 ;;
    --structs) struct_map "${2:?--structs needs a TYPE, e.g. FPN}"; exit 0 ;;
    --macros)  macro_map "${2:?--macros needs a TYPE, e.g. FPN}"; exit 0 ;;
    --full)    struct_map "${2:?--full needs a TYPE, e.g. FPN}"; echo; macro_map "${2}"; echo; type_map "${2}"; exit 0 ;;
    --functions) shift ;;
esac

OUT="${1:-$REPO_ROOT/DOCS/CODE_MAP.md}"

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
