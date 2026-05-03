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
# v5.9.3b — added --feature-scaler-present + --scaler-sha256 for the
# v5.9.3a scaler sidecar binding.
# v5.9.4a — added --model-num-outputs for the model-output-dimension
# stamp binding (catches multiclass-vs-binary mismatch at load).
# v5.9.5c — added 9 more flags for full inference cfg parity with the
# in-process emit at Backtest_RunFullValidation. Closes the
# half-wired StampInferenceCfgInputs gap caught at v5.9.5 sprint exit.
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
#       [--engine-version 5.9.5c] \
#       [--feature-scaler-present 1 --scaler-sha256 <hex>] \
#       [--model-num-outputs 3] \
#       [--confidence-threshold-scale 2.0] \
#       [--barrier-gate-enabled 1] \
#       [--confidence-hard-block-threshold 0.05] \
#       [--held-out-fraction 0.20] \
#       [--freshness-tau 300] \
#       [--bandit-blend-ratio 0.30] \
#       [--fee-rate-maker 0.00075 --fee-rate-taker 0.00100] \
#       [--training-poll-interval 100] \
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
FEATURE_SCALER_PRESENT=""
SCALER_SHA256=""
MODEL_NUM_OUTPUTS=""
# v5.9.5c — full inference cfg binding (CLI parity with in-process emit)
CONF_THR_SCALE=""
BARRIER_GATE=""
CONF_HARD_BLOCK=""
HELD_OUT_FRACTION=""
FRESHNESS_TAU=""
BANDIT_BLEND=""
FEE_RATE_MAKER=""
FEE_RATE_TAKER=""
TRAINING_POLL_INTERVAL=""
# v5.9.5h — XGBoost hyperparam defaults
XGB_MAX_DEPTH=""
XGB_LEARNING_RATE=""
XGB_N_ESTIMATORS=""
XGB_SUBSAMPLE=""
XGB_COLSAMPLE_BYTREE=""
XGB_MIN_CHILD_WEIGHT=""
XGB_SEED=""
XGB_TREE_METHOD=""
BUILD_FLAGS_HASH_HEX=""
FORCE=0

usage() {
    sed -n '4,52p' "$0"
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
        --feature-scaler-present) FEATURE_SCALER_PRESENT="$2"; shift 2 ;;
        --scaler-sha256)          SCALER_SHA256="$2";         shift 2 ;;
        --model-num-outputs)      MODEL_NUM_OUTPUTS="$2";     shift 2 ;;
        # v5.9.5c — full inference cfg binding (CLI parity with in-process emit)
        --confidence-threshold-scale)        CONF_THR_SCALE="$2";       shift 2 ;;
        --barrier-gate-enabled)              BARRIER_GATE="$2";          shift 2 ;;
        --confidence-hard-block-threshold)   CONF_HARD_BLOCK="$2";       shift 2 ;;
        --held-out-fraction)                 HELD_OUT_FRACTION="$2";     shift 2 ;;
        --freshness-tau)                     FRESHNESS_TAU="$2";         shift 2 ;;
        --bandit-blend-ratio)                BANDIT_BLEND="$2";          shift 2 ;;
        --fee-rate-maker)                    FEE_RATE_MAKER="$2";        shift 2 ;;
        --fee-rate-taker)                    FEE_RATE_TAKER="$2";        shift 2 ;;
        --training-poll-interval)            TRAINING_POLL_INTERVAL="$2"; shift 2 ;;
        # v5.9.5h — XGBoost hyperparam flags (8 total)
        --xgb-max-depth)                     XGB_MAX_DEPTH="$2";          shift 2 ;;
        --xgb-learning-rate)                 XGB_LEARNING_RATE="$2";      shift 2 ;;
        --xgb-n-estimators)                  XGB_N_ESTIMATORS="$2";       shift 2 ;;
        --xgb-subsample)                     XGB_SUBSAMPLE="$2";          shift 2 ;;
        --xgb-colsample-bytree)              XGB_COLSAMPLE_BYTREE="$2";   shift 2 ;;
        --xgb-min-child-weight)              XGB_MIN_CHILD_WEIGHT="$2";   shift 2 ;;
        --xgb-seed)                          XGB_SEED="$2";               shift 2 ;;
        --xgb-tree-method)                   XGB_TREE_METHOD="$2";        shift 2 ;;
        # v5.9.5h Phase 10 — build flags fingerprint
        --build-flags-hash)                  BUILD_FLAGS_HASH_HEX="$2";   shift 2 ;;
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

# v5.9.5c — inference cfg binding. Order MUST match in-process emitter at
# ML_Headers/ModelInference.hpp:1158-1191 byte-for-byte (HMAC verifies the
# entire body, signatures diverge on any byte difference). Five core fields
# emit together when ANY of the five is supplied (mirrors has_inference_cfg
# semantics: cfg-recording flag for "trainer recorded these"). Bandit +
# fees emit separately when their respective groups are supplied. Doubles
# formatted via printf "%g" to match in-process; LC_NUMERIC=C already
# pinned at script top.
if [[ -n "$CONF_THR_SCALE" || -n "$BARRIER_GATE" || -n "$CONF_HARD_BLOCK" \
   || -n "$HELD_OUT_FRACTION" || -n "$FRESHNESS_TAU" ]]; then
    CTS_FMT=$(awk -v v="${CONF_THR_SCALE:-0}"     'BEGIN { printf "%g", v }')
    BG_FMT="${BARRIER_GATE:-0}"
    CHB_FMT=$(awk -v v="${CONF_HARD_BLOCK:-0}"    'BEGIN { printf "%g", v }')
    HOF_FMT=$(awk -v v="${HELD_OUT_FRACTION:-0}"  'BEGIN { printf "%g", v }')
    FTAU_FMT=$(awk -v v="${FRESHNESS_TAU:-0}"     'BEGIN { printf "%g", v }')
    CANONICAL="${CANONICAL}inference_cfg_confidence_threshold_scale=${CTS_FMT}
inference_cfg_barrier_gate_enabled=${BG_FMT}
inference_cfg_confidence_hard_block_threshold=${CHB_FMT}
inference_cfg_held_out_fraction=${HOF_FMT}
inference_cfg_freshness_tau=${FTAU_FMT}
"
fi

# v5.9.5c — bandit_blend_ratio (gated on bandit_enabled in-process).
if [[ -n "$BANDIT_BLEND" ]]; then
    BB_FMT=$(awk -v v="$BANDIT_BLEND" 'BEGIN { printf "%g", v }')
    CANONICAL="${CANONICAL}inference_cfg_bandit_blend_ratio=${BB_FMT}
"
fi

# v5.9.5c — fee_rate_maker + fee_rate_taker (paired; gated on cost_gate_enabled
# in-process). Both emit together — partial fees would diverge from in-process
# emit which uses a single snprintf for both lines.
if [[ -n "$FEE_RATE_MAKER" && -n "$FEE_RATE_TAKER" ]]; then
    FRM_FMT=$(awk -v v="$FEE_RATE_MAKER" 'BEGIN { printf "%g", v }')
    FRT_FMT=$(awk -v v="$FEE_RATE_TAKER" 'BEGIN { printf "%g", v }')
    CANONICAL="${CANONICAL}inference_cfg_fee_rate_maker=${FRM_FMT}
inference_cfg_fee_rate_taker=${FRT_FMT}
"
elif [[ -n "$FEE_RATE_MAKER" || -n "$FEE_RATE_TAKER" ]]; then
    echo "[stamp] WARN: --fee-rate-maker + --fee-rate-taker must both be set; skipping fees" >&2
fi

# v5.9.5c — training_poll_interval (cadence binding). uint32_t printed as %u.
if [[ -n "$TRAINING_POLL_INTERVAL" ]]; then
    CANONICAL="${CANONICAL}training_poll_interval=${TRAINING_POLL_INTERVAL}
"
fi

# v5.9.3b — append feature_scaler_present + scaler_sha256 if both supplied.
# These bind the .scaler sidecar to the model. Order MUST match the
# in-process emitter at ML_Headers/ModelInference.hpp:1158-1167 (single
# block emit, both lines together). Bash-parity regression test pins
# this in controller_test.cpp.
if [[ -n "$FEATURE_SCALER_PRESENT" ]]; then
    CANONICAL="${CANONICAL}feature_scaler_present=${FEATURE_SCALER_PRESENT}
scaler_sha256=${SCALER_SHA256}
"
fi

# v5.9.4a — append model_num_outputs if supplied. Stamp records the
# trainer's num_outputs claim; engine load compares vs ModelHandle.
# Order MUST match in-process emitter at ML_Headers/ModelInference.hpp.
if [[ -n "$MODEL_NUM_OUTPUTS" ]]; then
    CANONICAL="${CANONICAL}model_num_outputs=${MODEL_NUM_OUTPUTS}
"
fi

# v5.9.5h — XGBoost hyperparams (canonical body position 17). All 8
# fields emit together as a block when ANY is supplied (matches
# in-process has_xgb_hyperparams gate at ModelInference.hpp:~1230).
# Order MUST match in-process emitter byte-for-byte (HMAC verify
# checks the entire canonical body).
if [[ -n "$XGB_MAX_DEPTH" || -n "$XGB_LEARNING_RATE" || -n "$XGB_N_ESTIMATORS" \
   || -n "$XGB_SUBSAMPLE" || -n "$XGB_COLSAMPLE_BYTREE" || -n "$XGB_MIN_CHILD_WEIGHT" \
   || -n "$XGB_SEED" || -n "$XGB_TREE_METHOD" ]]; then
    XMD="${XGB_MAX_DEPTH:-6}"
    XLR_FMT=$(awk -v v="${XGB_LEARNING_RATE:-0.1}"     'BEGIN { printf "%g", v }')
    XNE="${XGB_N_ESTIMATORS:-200}"
    XSS_FMT=$(awk -v v="${XGB_SUBSAMPLE:-0.8}"          'BEGIN { printf "%g", v }')
    XCT_FMT=$(awk -v v="${XGB_COLSAMPLE_BYTREE:-0.8}"   'BEGIN { printf "%g", v }')
    XMC="${XGB_MIN_CHILD_WEIGHT:-5}"
    XSD="${XGB_SEED:-42}"
    XTM="${XGB_TREE_METHOD:-hist}"
    CANONICAL="${CANONICAL}xgb_max_depth=${XMD}
xgb_learning_rate=${XLR_FMT}
xgb_n_estimators=${XNE}
xgb_subsample=${XSS_FMT}
xgb_colsample_bytree=${XCT_FMT}
xgb_min_child_weight=${XMC}
xgb_seed=${XSD}
xgb_tree_method=${XTM}
"
fi

# v5.9.5h Phase 10 — build flags fingerprint (canonical body position 18).
# Trainer's compile-time BUILD_FLAGS_HASH() in hex. Engine load-WARN
# compares stamp's hash vs current build's at boot.
if [[ -n "$BUILD_FLAGS_HASH_HEX" ]]; then
    CANONICAL="${CANONICAL}build_flags_hash=${BUILD_FLAGS_HASH_HEX}
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
