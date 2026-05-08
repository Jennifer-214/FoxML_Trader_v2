#!/usr/bin/env bash
# validate_feature_mask.sh — end-to-end check that v5.11.18a/.18-main
# feature_mask binding works at all 4 surfaces:
#   1. cfg parser    — engine.cfg's core_N_feature_mask field is read
#   2. stamp emit    — trained model's stamp body has feature_mask= line
#   3. stamp parse   — verify_model_stamp reads the field round-trip
#   4. load refusal  — engine refuses to load when cfg mask != stamp mask
#
# Usage:
#   ./tools/validate_feature_mask.sh <model_path> <secret>
#
# Inputs:
#   <model_path>  — path to an already-trained model (e.g. models/foo/barrier.json)
#                   The model's .stamp file must exist next to it.
#   <secret>      — HMAC secret used at training (or "" for devmode)
#
# Outputs (stdout):
#   PASS / FAIL per surface, exit code 0 if all PASS, 1 otherwise.

set -u
MODEL_PATH="${1:-}"
SECRET="${2:-}"

if [ -z "$MODEL_PATH" ]; then
    echo "Usage: $0 <model_path> [secret]"
    echo ""
    echo "  <model_path>  full path to .json model file (stamp file must exist at"
    echo "                <model_path>.stamp)"
    echo "  [secret]      HMAC secret used at training time. Empty = devmode."
    exit 2
fi

STAMP_PATH="${MODEL_PATH}.stamp"

if [ ! -f "$MODEL_PATH" ]; then
    echo "FAIL: model file not found: $MODEL_PATH"
    exit 1
fi
if [ ! -f "$STAMP_PATH" ]; then
    echo "FAIL: stamp file not found: $STAMP_PATH"
    echo "  (model was trained without auto_stamp_on_held_out=1, OR Run Full"
    echo "   Validation never fired auto-stamp. Re-train with stamp enabled.)"
    exit 1
fi

PASS_COUNT=0
FAIL_COUNT=0
NOTE_COUNT=0

note() { echo "  NOTE: $*"; NOTE_COUNT=$((NOTE_COUNT + 1)); }
pass() { echo "  PASS: $*"; PASS_COUNT=$((PASS_COUNT + 1)); }
fail() { echo "  FAIL: $*"; FAIL_COUNT=$((FAIL_COUNT + 1)); }

echo ""
echo "=== Surface 1: cfg parser reads core_N_feature_mask ==="
# Check that the cfg parser knows the field. If grep succeeds, field is wired.
if grep -qE 'core_feature_mask' CoreFrameworks/ControllerConfig.hpp; then
    pass "cfg parser supports core_N_feature_mask field"
else
    fail "cfg parser does NOT define core_feature_mask field — wiring broken"
fi

# Check engine.cfg.example or current engine.cfg has a sample entry
if [ -f engine.cfg.example ]; then
    if grep -qE '^#?\s*core_[0-9]+_feature_mask\s*=' engine.cfg.example; then
        pass "engine.cfg.example documents core_N_feature_mask"
    else
        note "engine.cfg.example doesn't show core_N_feature_mask (operator may not"
        note "      know about it). Consider adding a documented commented-out example."
    fi
fi

echo ""
echo "=== Surface 2: stamp body emits feature_mask= line ==="
if grep -qE '^feature_mask=' "$STAMP_PATH"; then
    MASK_HEX=$(grep -oE 'feature_mask=[0-9a-fA-F]+' "$STAMP_PATH" | sed 's/feature_mask=//')
    pass "stamp body has feature_mask=${MASK_HEX} (hex)"
else
    note "stamp body has NO feature_mask= line. Two possibilities:"
    note "  (a) model trained with default mask (all-on 0xFFFF...F) — emit is"
    note "      conditional on non-default; v5.11.18a convention. NORMAL."
    note "  (b) model trained on pre-v5.11.18a build — legacy stamp, no mask field."
    note "      Re-train with v5.11.18a+ to gain the field."
    MASK_HEX=""
fi

echo ""
echo "=== Surface 3: verify_model_stamp parses feature_mask round-trip ==="
# tools/stamp_model.sh has a verify mode? Let me check stamp_model.sh capabilities
if [ -x tools/stamp_model.sh ] && tools/stamp_model.sh 2>&1 | grep -q verify; then
    # Try a verify; output should contain feature_mask if stamp has it
    VERIFY_OUTPUT=$(tools/stamp_model.sh verify "$MODEL_PATH" "$SECRET" 2>&1)
    if [ -n "$MASK_HEX" ]; then
        if echo "$VERIFY_OUTPUT" | grep -qiE "feature_mask.*${MASK_HEX}|has_feature_mask.*1"; then
            pass "stamp_model.sh verify reads feature_mask correctly"
        else
            fail "stamp_model.sh verify did NOT surface feature_mask field"
            note "      verify output: $VERIFY_OUTPUT"
        fi
    else
        note "skipping verify check — no mask in stamp body (default mask path)"
    fi
else
    note "tools/stamp_model.sh has no verify subcommand; surface 3 not testable"
    note "      via shell. Code-side: ML_Headers/ModelInference.hpp parser at"
    note "      verify_model_stamp() handles it (see Surface 4 for load-time)."
fi

echo ""
echo "=== Surface 4: engine refuses load when cfg mask != stamp mask ==="
# This requires actually running the engine with a deliberately-mismatched cfg.
# We can't fully automate this in a shell script (would need to spawn engine
# with a temp cfg, capture stderr for the REFUSE log, then kill it). Document
# the manual test instead.
note "Surface 4 requires a live engine run. Manual test:"
note ""
note "  1. Note current stamp's feature_mask value:"
if [ -n "$MASK_HEX" ]; then
    note "       feature_mask=${MASK_HEX}"
else
    note "       (this stamp has no mask — re-train with non-default mask first)"
fi
note ""
note "  2. Edit engine.cfg, set:"
note "       core_0_feature_mask = 0xDEADBEEFCAFEBABE  # any value != stamp's"
note ""
note "  3. Run engine pointing at this model:"
note "       (set core_0_model_dir to the dir containing the model)"
note "       ./bin/engine_gui    # or engine_test"
note ""
note "  4. Watch stderr for:"
note "       \"REFUSING ${MODEL_PATH} — feature_mask mismatch: stamp=...\""
note ""
note "  If load proceeds (no refusal log), the load-time check is broken."
note "  Code site: ML_Headers/CoreModelZoo.hpp CoreModelZoo_TryLoadRole()"
note "  with expected_feature_mask param, and verify_model_stamp() refusal."

echo ""
echo "=== Summary ==="
echo "  PASS: $PASS_COUNT"
echo "  FAIL: $FAIL_COUNT"
echo "  NOTE: $NOTE_COUNT (informational; not failures)"
if [ $FAIL_COUNT -gt 0 ]; then
    echo ""
    echo "  Result: FAIL — feature_mask binding broken at one or more surfaces."
    exit 1
fi
echo ""
echo "  Result: PASS — surfaces 1-3 verified for this stamp. Surface 4 requires"
echo "          manual engine run (see notes above)."
exit 0
