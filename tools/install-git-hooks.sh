#!/bin/bash
# install-git-hooks.sh — install local-repo git hooks (NOT tracked by git natively).
#
# Run after fresh clone OR after editing a versioned hook to keep .git/hooks/ in sync
# with the versioned hook scripts at tools/hooks/.
#
# CANONICAL-SOURCE DISCIPLINE: tools/hooks/<name> is the SINGLE SOURCE OF TRUTH.
# .git/hooks/<name> is a COPY installed from it. NEVER edit .git/hooks/ directly —
# edit tools/hooks/<name> then re-run this script. (Drift between the two went
# undetected from .D to .D.1: Check 11 was added to the installed pre-commit hook but
# not synced back to the versioned source, so a fresh clone would have installed an
# outdated hook. Closed at .D.1 Phase A.3; cmp-parity now verified at install time.)
#
# Idempotent: replaces existing hooks of the same name (backs up to .bak if differ).
#
# Codified at v5.15.5.F.4d.1.B.4 v1.7.4 with the B-Plus CI tool pre-commit hook.
# The pre-commit hook now runs a multi-check suite (see tools/hooks/pre-commit header
# for the per-check codification history); this installer copies whatever is in
# tools/hooks/ verbatim, so new checks land by editing the versioned hook + re-running.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOOKS_DIR="${REPO_ROOT}/.git/hooks"
SOURCE_DIR="${REPO_ROOT}/tools/hooks"

if [ ! -d "$HOOKS_DIR" ]; then
    echo "[install-git-hooks] ERROR: $HOOKS_DIR not found. Run from inside a git checkout."
    exit 1
fi

if [ ! -d "$SOURCE_DIR" ]; then
    echo "[install-git-hooks] ERROR: $SOURCE_DIR not found. Are you on the right branch?"
    exit 1
fi

INSTALLED=0
SKIPPED=0
for src in "$SOURCE_DIR"/*; do
    [ -f "$src" ] || continue
    name=$(basename "$src")
    dst="${HOOKS_DIR}/${name}"
    if [ -f "$dst" ] && ! cmp -s "$src" "$dst"; then
        echo "[install-git-hooks] WARN: $name exists at $dst and differs from source."
        echo "[install-git-hooks]       Backing up to $dst.bak; installing new version."
        mv "$dst" "$dst.bak"
    fi
    cp "$src" "$dst"
    chmod +x "$dst"
    INSTALLED=$((INSTALLED + 1))
    echo "[install-git-hooks] Installed: $name"
done

echo "[install-git-hooks] Done. $INSTALLED hook(s) installed."
echo "[install-git-hooks] Note: hooks live in .git/hooks/ which is NOT tracked by git."
echo "[install-git-hooks] Re-run this script after pulling hook changes from tools/hooks/."
