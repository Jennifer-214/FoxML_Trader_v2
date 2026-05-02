#!/bin/bash
# stamp_model.sh — generate a held-out validation stamp for a trained model.
#
# Output: writes <model>.stamp alongside the model file. Format matches
# what `verify_model_stamp` in ML_Headers/ModelInference.hpp reads.
#
# Workflow assumption: you've already run walk-forward + held-out
# validation in foxml_suite (or via Backtest_RunFullValidation in tests)
# and have the metric numbers in hand. This script just signs them.
#
# v5.2.3 (Phase 1) — bash wrapper. Phase 2 (v5.3.x?) replaces this with
# tools/stamp_model.cpp that runs validation directly from CLI.
# v5.8.8 — added --feature-registry-hash and --engine-version to match
# the v5.8.6 in-process stamp body extension. Bash-signed stamps now
# verify identically to in-process-signed ones (regression-tested in
# controller_test.cpp).
#
# Usage:
#   ./tools/stamp_model.sh \
#       --model models/aggressive/buy_signal.bin \
#       --secret "$HELD_OUT_STAMP_SECRET" \
#       --wf-mean-val 0.55 \
#       --held-out-metric 0.53 \
#       --gap-threshold 0.05 \
#       [--trained-on 2026-04-29] \
#       [--format-version 5] \
#       [--feature-registry-hash fc9119b8ed47bcf9] \
#       [--engine-version 5.8.8] \
#       [--force]
#
# Refuses to write if gap > gap_threshold unless --force is passed.
# `--force` writes a stamp the engine will REJECT — use only for testing
# the gate itself.

set -euo pipefail

# v5.8.8 — pin LC_NUMERIC=C so awk's "%.6f" emits decimal-point format
# regardless of operator locale. Without this, awk under LC_NUMERIC=de_DE
# would write "0,500000" instead of "0.500000", breaking signature
# verification on the in-process side (which pins C internally via
# uselocale()).
export LC_NUMERIC=C

MODEL=""
SECRET=""
WF_MEAN_VAL=""
HELD_OUT=""
GAP_THRESHOLD=""
TRAINED_ON="$(date -u +%Y-%m-%d)"
FORMAT_VERSION="5"
FEATURE_REGISTRY_HASH=""
ENGINE_VERSION=""
FORCE=0

usage() {
    sed -n '4,38p' "$0"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --model)                  MODEL="$2";                 shift 2 ;;
        --secret)                 SECRET="$2";                shift 2 ;;
        --wf-mean-val)            WF_MEAN_VAL="$2";           shift 2 ;;
        --held-out-metric)        HELD_OUT="$2";              shift 2 ;;
        --gap-threshold)          GAP_THRESHOLD="$2";         shift 2 ;;
        --trained-on)             TRAINED_ON="$2";            shift 2 ;;
        --format-version)         FORMAT_VERSION="$2";        shift 2 ;;
        --feature-registry-hash)  FEATURE_REGISTRY_HASH="$2"; shift 2 ;;
        --engine-version)         ENGINE_VERSION="$2";        shift 2 ;;
        --force)                  FORCE=1;                    shift ;;
        -h|--help)                usage ;;
        *) echo "[stamp] unknown arg: $1" >&2; usage ;;
    esac
done

if [[ -z "$MODEL" || -z "$WF_MEAN_VAL" || -z "$HELD_OUT" || -z "$GAP_THRESHOLD" ]]; then
    echo "[stamp] missing required arg" >&2
    usage
fi
if [[ ! -f "$MODEL" ]]; then
    echo "[stamp] model file not found: $MODEL" >&2
    exit 2
fi

# 1. Compute model SHA-256
MODEL_SHA="$(sha256sum "$MODEL" | awk '{print $1}')"
if [[ -z "$MODEL_SHA" ]]; then
    echo "[stamp] failed to compute SHA-256 of $MODEL" >&2
    exit 3
fi

# 2. Compute gap = abs(wf - held_out)
GAP="$(awk -v a="$WF_MEAN_VAL" -v b="$HELD_OUT" 'BEGIN { d=a-b; if (d<0) d=-d; printf "%.6f", d }')"

# 3. Refuse on gap > threshold (unless --force)
GAP_OK="$(awk -v g="$GAP" -v t="$GAP_THRESHOLD" 'BEGIN { print (g <= t) ? 1 : 0 }')"
if [[ "$GAP_OK" == "0" && "$FORCE" == "0" ]]; then
    echo "[stamp] REFUSE: gap $GAP > threshold $GAP_THRESHOLD" >&2
    echo "[stamp]         model is overfit per held-out check; do not deploy." >&2
    echo "[stamp]         pass --force to write the stamp anyway (engine will reject)" >&2
    exit 4
fi

# 4. Build canonical body — must match verify_model_stamp's parser
#    byte-for-byte (HMAC verifies the entire body before signature=).
#    Order: format-version, sha256, trained_on, wf_mean_val, held_out_metric,
#           gap, gap_threshold, [feature_registry_hash], [engine_version].
#    feature_registry_hash + engine_version appended ONLY when format_version >= 5
#    AND non-empty value supplied — matches the in-process stamp_write_for_model
#    conditional at ML_Headers/ModelInference.hpp.
CANONICAL="$(cat <<EOF
model_format_version=${FORMAT_VERSION}
model_sha256=${MODEL_SHA}
trained_on=${TRAINED_ON}
wf_mean_val=${WF_MEAN_VAL}
held_out_metric=${HELD_OUT}
gap=${GAP}
gap_threshold=${GAP_THRESHOLD}
EOF
)"
# Trailing newline matters — verify side does the same
CANONICAL="${CANONICAL}
"

# v5.8.8 — append feature_registry_hash if format >= 5 and value supplied
if [[ "$FORMAT_VERSION" -ge 5 && -n "$FEATURE_REGISTRY_HASH" ]]; then
    CANONICAL="${CANONICAL}feature_registry_hash=${FEATURE_REGISTRY_HASH}
"
fi

# v5.8.8 — append engine_version if format >= 5 and value supplied
if [[ "$FORMAT_VERSION" -ge 5 && -n "$ENGINE_VERSION" ]]; then
    CANONICAL="${CANONICAL}engine_version=${ENGINE_VERSION}
"
fi

# v5.9.0 — append stamp_format_version when format >= 5. Schema version of
# the stamp body itself (distinct from model_format_version which versions
# the model file shape). Bumped on future stamp body schema changes.
# Verifier records the value; future strict mode could reject unknown
# versions. MUST stay in sync with stamp_write_for_model in
# ML_Headers/ModelInference.hpp — bash-parity regression test pins this.
if [[ "$FORMAT_VERSION" -ge 5 ]]; then
    CANONICAL="${CANONICAL}stamp_format_version=1
"
fi

# 5. HMAC-SHA256(secret, canonical_body)
if [[ -z "$SECRET" ]]; then
    # Empty secret = dev mode (verify_model_stamp accepts any sig).
    # Write a placeholder so the file is well-formed.
    SIG="devmode-no-secret-no-signature"
    echo "[stamp] WARN: --secret empty — engine will load in dev mode (sig unchecked)" >&2
else
    SIG="$(printf '%s' "$CANONICAL" | openssl dgst -sha256 -hmac "$SECRET" 2>/dev/null | awk '{print $NF}')"
    if [[ -z "$SIG" ]]; then
        echo "[stamp] failed to compute HMAC-SHA256 (openssl error)" >&2
        exit 5
    fi
fi

# 6. Write <model>.stamp
STAMP_PATH="${MODEL}.stamp"
{
    printf '%s' "$CANONICAL"
    printf 'signature=%s\n' "$SIG"
} > "$STAMP_PATH"

# 7. Done — log summary
if [[ "$FORCE" == "1" && "$GAP_OK" == "0" ]]; then
    echo "[stamp] WROTE (with --force, gate will REJECT): $STAMP_PATH"
    echo "[stamp]   gap=$GAP threshold=$GAP_THRESHOLD format=v$FORMAT_VERSION"
else
    echo "[stamp] OK: $STAMP_PATH"
    echo "[stamp]   model_sha256=${MODEL_SHA:0:16}..."
    echo "[stamp]   wf_mean_val=$WF_MEAN_VAL  held_out=$HELD_OUT  gap=$GAP  threshold=$GAP_THRESHOLD"
    if [[ -n "$FEATURE_REGISTRY_HASH" ]]; then
        echo "[stamp]   feature_registry_hash=$FEATURE_REGISTRY_HASH"
    fi
    if [[ -n "$ENGINE_VERSION" ]]; then
        echo "[stamp]   engine_version=$ENGINE_VERSION"
    fi
    echo "[stamp]   signature=${SIG:0:16}..."
fi
