#!/bin/bash
# check_session_docs.sh — ONE mechanical sweep of all DOC/PLAN CI checks.
#
# WHY THIS EXISTS (codified v5.15.5.F.4d.1.E Session-4, 2026-05-30):
# Plan bodies + memories live in the WORKSPACE repo (committed via /sync-workspace),
# but the pre-commit hook (B-Plus etc.) is installed in the ENGINE repo + plans/ is
# gitignored there → the engine hook NEVER gates workspace doc commits. The
# bidirectional-memories check isn't in the hook at all. Net result: NOTHING
# mechanical caught a broken plan-body citation (CoreFrameworks/ prefix) or a one-way
# memory sister-link this session — they survived until hand-run. This aggregator is
# the SINGLE command that runs every doc/plan CI tool, so "are the docs clean?" is a
# deterministic one-shot, not N hand-checks. Fired by /close-session Stage 0.
#
# It CALLS existing tools (no logic duplication — SSoT for "full doc sweep"):
#   check_doc_metadata.py --bidirectional --memories   [HARD — exit 1 on asymmetry/broken-ref]
#   check_plan_body_symbol_existence.py (B-Plus)        [HARD — exit 1 on fabrication]
#   check_forward_promise_audit.py                      [ADVISORY — MED/LOW expected]
#   check_meta_registry.py                              [ADVISORY — engine-structural; pre-existing orphans surfaced]
#
# Exit 0 = all HARD checks pass. Exit 1 = a HARD check failed (fix before declaring clean).
# Bypass a single check via the same SKIP_* env vars the pre-commit hook honors.
#
# Usage:
#   tools/check_session_docs.sh                 # B-Plus over session-modified workspace plan bodies
#   tools/check_session_docs.sh --all-plans     # B-Plus over ALL plan bodies (slower; full sweep)

set -uo pipefail
# Machine-portable roots (per feedback_machine_portable_resolver_for_committed_tool_paths):
# REPO_ROOT derives from this script's location (<engine>/tools/check_session_docs.sh);
# WORKSPACE_ROOT via env-override -> sibling-default. No $HOME hardcode in a committed,
# public-AGPL tool — runs on any clone / any PC / SSH-grid node.
REPO_ROOT="${FOXML_ENGINE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
if [ -n "${FOXML_WORKSPACE:-}" ] && [ -d "${FOXML_WORKSPACE}" ]; then
    WORKSPACE_ROOT="${FOXML_WORKSPACE}"
elif [ -d "$(dirname "$REPO_ROOT")/tick-trader-percore-workspace" ]; then
    WORKSPACE_ROOT="$(dirname "$REPO_ROOT")/tick-trader-percore-workspace"
else
    WORKSPACE_ROOT="$REPO_ROOT"
fi
ALL_PLANS=0
[ "${1:-}" = "--all-plans" ] && ALL_PLANS=1

HARD_FAIL=0
declare -a RESULTS

run_hard() { # name, cmd...
    local name="$1"; shift
    if "$@" >/tmp/csd_$$.log 2>&1; then
        RESULTS+=("  ✅ HARD  $name")
    else
        RESULTS+=("  ❌ HARD  $name  (exit $?) — see detail below")
        echo "----- $name detail -----"; cat /tmp/csd_$$.log
        HARD_FAIL=1
    fi
    rm -f /tmp/csd_$$.log
}
run_advisory() { # name, cmd...
    local name="$1"; shift
    if "$@" >/tmp/csd_$$.log 2>&1; then
        RESULTS+=("  ✅ ADV   $name")
    else
        RESULTS+=("  ⚠️  ADV   $name  (exit $?; advisory — not blocking)")
    fi
    rm -f /tmp/csd_$$.log
}

echo "=========================================================="
echo " check_session_docs.sh — mechanical doc/plan CI sweep"
echo "=========================================================="

# --- HARD 1: bidirectional + index sync over memories (the red-build catcher) ---
if [ "${SKIP_BIDIR_CHECK:-0}" != "1" ]; then
    run_hard "doc-metadata bidirectional+index (memories)" \
        python3 "$REPO_ROOT/tools/check_doc_metadata.py" --bidirectional --memories
else
    RESULTS+=("  ⏭  HARD  doc-metadata bidirectional (SKIP_BIDIR_CHECK=1)")
fi

# --- HARD 2: B-Plus plan-body symbol existence (the citation-error catcher) ---
if [ "${SKIP_PLAN_BODY_CHECK:-0}" != "1" ]; then
    B_PLUS="$REPO_ROOT/tools/check_plan_body_symbol_existence.py"
    if [ "$ALL_PLANS" = "1" ]; then
        run_hard "B-Plus (ALL plan bodies)" python3 "$B_PLUS" --all
    else
        # session-modified workspace plan/doc .md (precise scope; the close surface)
        MODIFIED=$(git -C "$WORKSPACE_ROOT" diff --name-only HEAD -- 'plans/**/*.md' 2>/dev/null; \
                   git -C "$WORKSPACE_ROOT" ls-files --others --exclude-standard -- 'plans/**/*.md' 2>/dev/null)
        MODIFIED=$(echo "$MODIFIED" | grep -E 'subplans/.*\.md$|MASTER\.md$' | sort -u || true)
        if [ -z "$MODIFIED" ]; then
            RESULTS+=("  ✅ HARD  B-Plus (no session-modified plan bodies)")
        else
            FAB=0
            for rel in $MODIFIED; do
                f="$WORKSPACE_ROOT/$rel"
                [ -f "$f" ] || continue
                python3 "$B_PLUS" "$f" >/tmp/csd_bp_$$.log 2>&1 || FAB=1
            done
            if [ "$FAB" = "1" ]; then
                RESULTS+=("  ❌ HARD  B-Plus (session plan bodies) — fabrication/missing citation")
                echo "----- B-Plus detail (last) -----"; cat /tmp/csd_bp_$$.log
                HARD_FAIL=1
            else
                RESULTS+=("  ✅ HARD  B-Plus (session plan bodies)")
            fi
            rm -f /tmp/csd_bp_$$.log
        fi
    fi
else
    RESULTS+=("  ⏭  HARD  B-Plus (SKIP_PLAN_BODY_CHECK=1)")
fi

# --- HARD 3: capture-audit mechanical checks (index-sync / sentinels / skill-linkage) ---
if [ "${SKIP_CAPTURE_AUDIT_CHECK:-0}" != "1" ]; then
    run_hard "capture-audit mechanical (index/sentinels/skill-linkage)" \
        python3 "$REPO_ROOT/tools/check_capture_audit.py" --quiet
else
    RESULTS+=("  ⏭  HARD  capture-audit mechanical (SKIP_CAPTURE_AUDIT_CHECK=1)")
fi

# --- ADVISORY: forward-promise (MED/LOW backlog expected) ---
run_advisory "forward-promise audit (--since HEAD~5)" \
    python3 "$REPO_ROOT/tools/check_forward_promise_audit.py" --since HEAD~5

# --- ADVISORY: meta-registry orphans (engine-structural; pre-existing surfaced) ---
run_advisory "meta-registry coverage" \
    python3 "$REPO_ROOT/tools/check_meta_registry.py"

echo ""
echo "=== SWEEP RESULTS ==="
for r in "${RESULTS[@]}"; do echo "$r"; done
echo ""
if [ "$HARD_FAIL" = "1" ]; then
    echo "❌ SWEEP FAILED — a HARD doc/plan check failed. Fix before declaring clean."
    exit 1
fi
echo "✅ SWEEP CLEAN — all HARD doc/plan checks pass (advisories may be non-zero; see above)."
exit 0
