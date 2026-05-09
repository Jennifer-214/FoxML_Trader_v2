#!/usr/bin/env python3
"""
v5.14.3.A — feature_overlay.py — overlay-patch sidecar writer for
3-layer registry fingerprinting.

PURPOSE:
    Operators training models with feature overrides (per-target
    `lookback_override`, `disabled` flags, etc.) emit an
    `<model>.overlay.json` sidecar that the engine reads at load time.
    The sidecar's SHA256 is folded into the model's stamp body
    (`overlay_hash` field; layer-2). A composite SHA256(layer1 || layer2)
    becomes the `effective_hash` (layer-3) — the canonical fingerprint
    that uniquely identifies the model + overlay combination.

LAYERED FINGERPRINTING:
    layer-1 = base FEATURE_REGISTRY_HASH (existing C++ FNV-1a; unchanged)
    layer-2 = SHA256(canonical(overlay JSON))            ← THIS FILE writes
    layer-3 = SHA256(layer-1 hex || layer-2 hex)         ← THIS FILE writes

    Engine load-time verification:
    - Compares stamp's overlay_hash vs computed layer-2 from sidecar on disk
    - If sidecar missing but stamp claims overlay → REFUSE (strict) / WARN (loose)
    - If sidecar present + hash matches → accept
    - If hash mismatch → REFUSE (strict) / WARN (loose)

CANONICAL JSON ORDERING:
    Keys sorted lexicographically; nested arrays preserve order;
    no whitespace except after commas/colons; UTF-8 encoded; LF-terminated.
    Identical JSON content (regardless of write-time formatting) produces
    identical layer-2 hash. Required for stamp body bytewise determinism
    + cross-platform reproducibility.

USAGE:
    # Compute layer-2 + layer-3, write sidecar:
    feature_overlay.py write \\
        --base-hash 0x<layer1_hex> \\
        --overlays-json overlays.json \\
        --out path/to/model.overlay.json

    # Inspect (compute hashes from existing sidecar):
    feature_overlay.py inspect path/to/model.overlay.json

    # Verify against stamp body (operator self-check):
    feature_overlay.py verify \\
        --sidecar path/to/model.overlay.json \\
        --stamp-overlay-hash <hex> \\
        --stamp-effective-hash <hex>

OVERLAY JSON SHAPE (input to `write`):
    {
      "version": 1,
      "overlays": [
        {"feature_name": "ror_slope", "lookback_override": 256},
        {"feature_name": "vwap_dev",  "disabled": true}
      ]
    }

    Fields recognized:
    - feature_name: string (must match a registered feature in C++ side)
    - lookback_override: int (per-feature lookback window)
    - disabled: bool (skip this feature entirely)
    - Additional fields: passed through (forward-compat for v5.X+ overlays)

WRITTEN SIDECAR SHAPE (`<model>.overlay.json`):
    {
      "version": 1,
      "base_registry_hash": "0x<layer1_hex>",
      "overlays": [...],
      "computed_layer2_hash": "<64-char hex>",
      "computed_layer3_hash": "<64-char hex>"
    }

EXIT CODES:
    0 = success
    1 = invalid input (JSON parse error, missing required fields)
    2 = file I/O error (can't read/write sidecar)
    3 = verify mismatch (hash differs from stamp body claim)
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any, Dict, Optional


SIDECAR_FORMAT_VERSION = 1


def canonicalize_overlay_json(data: Dict[str, Any]) -> str:
    """
    Produce canonical JSON string for hashing. Determinism rules:
    - Keys sorted lexicographically at every nesting level
    - No whitespace except `, ` and `: ` (json default separators=(', ', ': '))
    - UTF-8 encoded
    - LF newline at end (single trailing \\n for cross-platform stability)

    The base_registry_hash + computed_* fields are EXCLUDED from canonicalization
    (those are derived/written values; layer-2 hash is over the operator's
    overlay declaration only).
    """
    # Filter to overlay-content-only keys for hash input
    canonical_fields = {
        "version": data.get("version", SIDECAR_FORMAT_VERSION),
        "overlays": data.get("overlays", []),
    }
    return json.dumps(
        canonical_fields,
        sort_keys=True,
        separators=(",", ":"),  # tightest legal JSON
        ensure_ascii=False,
    )


def compute_layer2_hash(overlay_data: Dict[str, Any]) -> str:
    """SHA256 of canonical overlay JSON. Returns 64-char lowercase hex."""
    canonical = canonicalize_overlay_json(overlay_data)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def compute_layer3_hash(layer1_hex: str, layer2_hex: str) -> str:
    """
    Composite layer-3 hash: SHA256(layer1_hex || layer2_hex).
    Both inputs lowercase hex; output 64-char lowercase hex.
    Strips '0x' prefix from layer1 if present (FEATURE_REGISTRY_HASH
    output may include it).
    """
    l1 = layer1_hex.lower().lstrip("0x").zfill(16)  # FNV-1a is 16 hex chars
    l2 = layer2_hex.lower().lstrip("0x").zfill(64)  # SHA256 is 64 hex chars
    composite = (l1 + l2).encode("utf-8")
    return hashlib.sha256(composite).hexdigest()


def validate_overlay_input(data: Dict[str, Any]) -> Optional[str]:
    """
    Returns None on valid input; returns error string on invalid.
    Validates structural shape (operator-friendly errors).
    """
    if not isinstance(data, dict):
        return "overlay JSON root must be an object"
    if "overlays" not in data:
        return "overlay JSON missing required key 'overlays' (array)"
    overlays = data["overlays"]
    if not isinstance(overlays, list):
        return f"'overlays' must be an array, got {type(overlays).__name__}"
    for i, entry in enumerate(overlays):
        if not isinstance(entry, dict):
            return f"overlay[{i}] must be an object, got {type(entry).__name__}"
        if "feature_name" not in entry:
            return f"overlay[{i}] missing required key 'feature_name'"
        if not isinstance(entry["feature_name"], str):
            return f"overlay[{i}].feature_name must be a string"
        if not entry["feature_name"]:
            return f"overlay[{i}].feature_name must be non-empty"
        # lookback_override is optional but must be int if present
        if "lookback_override" in entry:
            if not isinstance(entry["lookback_override"], int):
                return f"overlay[{i}].lookback_override must be int"
            if entry["lookback_override"] <= 0:
                return f"overlay[{i}].lookback_override must be > 0"
        # disabled is optional but must be bool if present
        if "disabled" in entry and not isinstance(entry["disabled"], bool):
            return f"overlay[{i}].disabled must be a boolean"
    # version optional; default 1
    if "version" in data and not isinstance(data["version"], int):
        return "'version' must be an integer"
    return None


def cmd_write(args: argparse.Namespace) -> int:
    """Write sidecar with computed layer-2 + layer-3 hashes."""
    try:
        overlay_text = Path(args.overlays_json).read_text(encoding="utf-8")
    except OSError as e:
        print(f"ERROR: cannot read overlays JSON file: {e}", file=sys.stderr)
        return 2

    try:
        overlay_data = json.loads(overlay_text)
    except json.JSONDecodeError as e:
        print(f"ERROR: invalid JSON in overlays file: {e}", file=sys.stderr)
        return 1

    err = validate_overlay_input(overlay_data)
    if err:
        print(f"ERROR: invalid overlay shape: {err}", file=sys.stderr)
        return 1

    layer2 = compute_layer2_hash(overlay_data)
    layer3 = compute_layer3_hash(args.base_hash, layer2)

    sidecar = {
        "version": overlay_data.get("version", SIDECAR_FORMAT_VERSION),
        "base_registry_hash": args.base_hash.lower(),
        "overlays": overlay_data["overlays"],
        "computed_layer2_hash": layer2,
        "computed_layer3_hash": layer3,
    }

    # Pretty-printed sidecar for operator inspection (NOT the canonical
    # form used for hashing — canonicalize_overlay_json reads sidecar's
    # `overlays` field + recomputes layer-2 to verify).
    sidecar_text = json.dumps(sidecar, indent=2, sort_keys=True) + "\n"

    try:
        # Atomic write: tmp + rename (matches stamp body discipline at
        # ML_Headers/ModelInference.hpp stamp_write_for_model)
        out_path = Path(args.out)
        tmp_path = out_path.with_suffix(out_path.suffix + ".tmp")
        tmp_path.write_text(sidecar_text, encoding="utf-8")
        tmp_path.replace(out_path)
    except OSError as e:
        print(f"ERROR: cannot write sidecar: {e}", file=sys.stderr)
        return 2

    print(f"OK — wrote {args.out}")
    print(f"  layer-2 (overlay):   {layer2}")
    print(f"  layer-3 (composite): {layer3}")
    print(f"  emit these to stamp body as:")
    print(f"    overlay_hash={layer2}")
    print(f"    effective_hash={layer3}")
    return 0


def cmd_inspect(args: argparse.Namespace) -> int:
    """Compute hashes from existing sidecar; useful for operator debugging."""
    try:
        sidecar_text = Path(args.sidecar).read_text(encoding="utf-8")
        sidecar = json.loads(sidecar_text)
    except (OSError, json.JSONDecodeError) as e:
        print(f"ERROR: cannot read/parse sidecar: {e}", file=sys.stderr)
        return 1

    layer2 = compute_layer2_hash(sidecar)
    base = sidecar.get("base_registry_hash", "0x0")
    layer3 = compute_layer3_hash(base, layer2)

    print(f"sidecar: {args.sidecar}")
    print(f"  base_registry_hash:   {base}")
    print(f"  computed_layer2_hash: {layer2}")
    print(f"  computed_layer3_hash: {layer3}")
    if "computed_layer2_hash" in sidecar:
        if sidecar["computed_layer2_hash"] != layer2:
            print(
                f"  WARNING: stored layer-2 ({sidecar['computed_layer2_hash']}) "
                f"DIFFERS from recomputed ({layer2})",
                file=sys.stderr,
            )
            return 3
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    """Verify sidecar against stamp body's claimed hashes."""
    try:
        sidecar_text = Path(args.sidecar).read_text(encoding="utf-8")
        sidecar = json.loads(sidecar_text)
    except (OSError, json.JSONDecodeError) as e:
        print(f"ERROR: cannot read/parse sidecar: {e}", file=sys.stderr)
        return 1

    layer2 = compute_layer2_hash(sidecar)
    base = sidecar.get("base_registry_hash", "0x0")
    layer3 = compute_layer3_hash(base, layer2)

    rc = 0
    if args.stamp_overlay_hash and args.stamp_overlay_hash.lower() != layer2:
        print(
            f"MISMATCH overlay_hash: stamp claims {args.stamp_overlay_hash} "
            f"but recomputed {layer2}",
            file=sys.stderr,
        )
        rc = 3
    if args.stamp_effective_hash and args.stamp_effective_hash.lower() != layer3:
        print(
            f"MISMATCH effective_hash: stamp claims {args.stamp_effective_hash} "
            f"but recomputed {layer3}",
            file=sys.stderr,
        )
        rc = 3
    if rc == 0:
        print(f"OK — sidecar matches stamp claims")
        print(f"  layer-2: {layer2}")
        print(f"  layer-3: {layer3}")
    return rc


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="v5.14.3.A — overlay-patch sidecar writer for 3-layer fingerprinting"
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_write = sub.add_parser("write", help="Write sidecar with computed hashes")
    p_write.add_argument("--base-hash", required=True,
                          help="Layer-1 FEATURE_REGISTRY_HASH (hex; with or without 0x prefix)")
    p_write.add_argument("--overlays-json", required=True,
                          help="Path to overlay declarations JSON file")
    p_write.add_argument("--out", required=True,
                          help="Output sidecar path (typically <model>.overlay.json)")
    p_write.set_defaults(func=cmd_write)

    p_inspect = sub.add_parser("inspect", help="Recompute hashes from existing sidecar")
    p_inspect.add_argument("sidecar", help="Path to sidecar JSON file")
    p_inspect.set_defaults(func=cmd_inspect)

    p_verify = sub.add_parser("verify", help="Verify sidecar against stamp body claims")
    p_verify.add_argument("--sidecar", required=True, help="Path to sidecar JSON")
    p_verify.add_argument("--stamp-overlay-hash", help="Stamp's overlay_hash claim")
    p_verify.add_argument("--stamp-effective-hash", help="Stamp's effective_hash claim")
    p_verify.set_defaults(func=cmd_verify)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
