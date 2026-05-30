#!/usr/bin/env python3
"""migrate_memory_frontmatter.py — bring Claude Code memories into the structured
doc-system (D-89 / TECH_DEBT-115), idempotently + non-destructively.

For each memory/*.md it:
  - derives `sister_specs` from inline [[links]] (filename-normalized -> fixes the
    WH-1 kebab-name-slug drift), UNION-merged with any existing sister_specs
    (never drops a hand-added one);
  - ensures a `metadata.tags:` field exists (adds `[]` if absent; never removes or
    overwrites an existing judgment tag list);
  - keeps doc-system fields nested under the harness-native `metadata:` block;
  - PRESERVES every other line byte-for-byte (name/description/type/node_type/
    originSessionId/unknown keys). This is the R6 void: the tool that touches the
    files cannot drop a field it does not manage.

Idempotent: re-running re-derives sisters from the body, so the field self-heals.
Safe: --dry-run prints a unified diff; --apply writes only changed files.

Reuses check_doc_metadata's memory-dir resolver (machine-portable; D-89 fork 1).

Usage:
  python3 tools/migrate_memory_frontmatter.py --dry-run            # diff all
  python3 tools/migrate_memory_frontmatter.py --dry-run --paths a.md b.md
  python3 tools/migrate_memory_frontmatter.py --apply              # write changes
  python3 tools/migrate_memory_frontmatter.py --report             # tags/links status

Exit: 0 ok (dangling [[links]] reported as INFO, non-fatal); 2 script error (no memory dir).
"""
import sys
import re
import argparse
import difflib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_doc_metadata import _resolve_memory_dir  # SSoT for the memory-dir path

LINK_RE = re.compile(r"\[\[([^\]]+)\]\]")
# top-level metadata keys that belong under the harness `metadata:` block when we
# normalize an older top-level-form (Form-A) file.
HARNESS_META_KEYS = ("type", "node_type", "originSessionId")


def split_frontmatter(text):
    """Return (fm_lines, body_text, ok). fm_lines excludes the --- markers."""
    if not text.startswith("---\n"):
        return None, text, False
    end = text.find("\n---\n", 4)
    if end == -1:
        return None, text, False
    fm = text[4:end]
    body = text[end + 5:]
    return fm.split("\n"), body, True


TYPE_PREFIXES = ("feedback_", "user_", "project_", "reference_")


def build_name_index(mem_dir):
    """stem/kebab/post-prefix variants -> filename.md, for WH-1 [[link]] resolution.

    Inline [[links]] often use the memory's `name:` slug, which frequently OMITS
    the type prefix and uses kebab-case (e.g. [[plan-right-not-fast]] for
    feedback_plan_right_not_fast.md). Index full stems first (priority); then the
    post-prefix slug via setdefault so a full-stem match always wins a collision.
    """
    idx = {}
    files = [f for f in sorted(mem_dir.glob("*.md")) if f.name != "MEMORY.md"]
    for f in files:                       # pass 1: full stems (highest priority)
        idx[f.stem] = f.name
        idx.setdefault(f.stem.replace("_", "-"), f.name)
    for f in files:                       # pass 2: post-prefix slug (fallback)
        for p in TYPE_PREFIXES:
            if f.stem.startswith(p):
                suf = f.stem[len(p):]
                idx.setdefault(suf, f.name)
                idx.setdefault(suf.replace("_", "-"), f.name)
                break
    return idx


def resolve_link(token, name_idx):
    """Map an inline [[token]] to a memory filename, or None if unresolvable."""
    t = token.strip().strip("/")
    if t.endswith(".md"):
        t = t[:-3]
    if t in name_idx:
        return name_idx[t]
    if t.replace("-", "_") in name_idx:
        return name_idx[t.replace("-", "_")]
    return None


def derive_sisters(body, name_idx):
    """Return (resolved_filenames_in_order, unresolved_tokens)."""
    resolved, unresolved, seen = [], [], set()
    for tok in LINK_RE.findall(body):
        fn = resolve_link(tok, name_idx)
        if fn is None:
            unresolved.append(tok)
        elif fn not in seen:
            seen.add(fn)
            resolved.append(fn)
    return resolved, unresolved


def collapse_block_lists(fm_lines):
    """Collapse block-style `key:` + indented `- item` lines into inline `key: [a, b]`.

    R6 repair (D-89): the Claude Code harness re-serializes agent-written memory
    frontmatter, turning inline lists into block style. Run this BEFORE the line-based
    transform so the helper sees a single canonical inline form — otherwise it appends
    a fresh inline line and leaves the block items as orphans (the duplication bug).
    """
    out, i, n = [], 0, len(fm_lines)
    while i < n:
        line = fm_lines[i]
        m = re.match(r"^(\s*)([A-Za-z0-9_]+):\s*$", line)
        if m:
            indent, key = m.group(1), m.group(2)
            items, j = [], i + 1
            while j < n:
                im = re.match(r"^(\s+)-\s+(.*\S)\s*$", fm_lines[j])
                if im and len(im.group(1)) > len(indent):
                    items.append(im.group(2).strip())
                    j += 1
                else:
                    break
            if items:                       # real block list -> inline (metadata: parent has no `- ` children, untouched)
                out.append(f"{indent}{key}: [{', '.join(items)}]")
                i = j
                continue
        out.append(line)
        i += 1
    return out


def _parse_list(val):
    val = val.strip()
    if val.startswith("[") and val.endswith("]"):
        return [i.strip() for i in val[1:-1].split(",") if i.strip()]
    return []


def _render_list(items):
    return "[" + ", ".join(items) + "]"


def set_tags_in_text(text, tags):
    """Replace the metadata.tags line with the given list. Returns (changed, new_text).

    Used by --set-tags (the judgment tag pass). Non-destructive: only the tags line
    changes; sister_specs + all other fields untouched.
    """
    fm_lines, body, ok = split_frontmatter(text)
    if not ok:
        return False, text
    fm_lines = collapse_block_lists(fm_lines)   # R6: canonicalize harness block-style -> inline
    out, done = [], False
    for line in fm_lines:
        m = re.match(r"^(\s*)tags:", line)
        if m and not done:
            out.append(f"{m.group(1)}tags: {_render_list(tags)}")
            done = True
        else:
            out.append(line)
    if not done:
        return False, text  # no tags line (pre-migration) — run the migration first
    new_text = "---\n" + "\n".join(out) + "\n---\n" + body
    return (new_text != text), new_text


def _find_metadata_block(fm_lines):
    """Return (meta_start, meta_end) indices of the metadata: block, or (None, None).

    meta_start = index of the `metadata:` line; meta_end = index one past the last
    indented sub-line (exclusive). Sub-lines are those indented beyond column 0.
    """
    for i, line in enumerate(fm_lines):
        if re.match(r"^metadata:\s*$", line):
            j = i + 1
            while j < len(fm_lines) and (fm_lines[j].startswith((" ", "\t")) or fm_lines[j] == ""):
                # stop at a blank line that precedes a non-indented line (end of block)
                if fm_lines[j] == "":
                    k = j + 1
                    while k < len(fm_lines) and fm_lines[k] == "":
                        k += 1
                    if k < len(fm_lines) and not fm_lines[k].startswith((" ", "\t")):
                        break
                j += 1
            return i, j
    return None, None


def transform_fm(fm_lines, new_sisters):
    """Return new fm_lines with metadata.tags ensured + metadata.sister_specs unioned.

    Non-destructive: only the two managed keys are inserted/updated; all other lines
    pass through. Handles both the metadata-block form and the older top-level form.
    """
    fm = list(fm_lines)
    m_start, m_end = _find_metadata_block(fm)

    if m_start is None:
        # Form-A (no metadata block): wrap harness keys under a new metadata: block.
        head, moved, rest = [], [], []
        for line in fm:
            key = line.split(":", 1)[0].strip() if ":" in line else ""
            if key in HARNESS_META_KEYS:
                moved.append("  " + line.strip())
            elif line.strip() == "":
                rest.append(line)
            else:
                head.append(line)
        block = ["metadata:"] + moved
        if not any(re.match(r"^\s*tags:", x) for x in block):
            block.append("  tags: []")
        block.append("  sister_specs: " + _render_list(new_sisters))
        # head keeps name/description (+ any non-harness top-level keys); drop stray blanks
        head = [h for h in head if h.strip() != ""]
        return head + block

    # metadata-block form: operate within [m_start+1, m_end)
    indent = "  "
    for k in range(m_start + 1, m_end):
        mm = re.match(r"^(\s+)\S", fm[k])
        if mm:
            indent = mm.group(1)
            break

    block = fm[m_start:m_end]
    has_tags = any(re.match(r"^\s*tags:", b) for b in block)

    # union sister_specs (replace in place if present, else append)
    existing_sisters = []
    sister_idx = None
    for bi, b in enumerate(block):
        if re.match(r"^\s*sister_specs:", b):
            sister_idx = bi
            existing_sisters = _parse_list(b.split(":", 1)[1])
            break
    union = list(existing_sisters)
    for s in new_sisters:
        if s not in union:
            union.append(s)
    sister_line = indent + "sister_specs: " + _render_list(union)

    if sister_idx is not None:
        block[sister_idx] = sister_line
    else:
        block.append(sister_line)
    if not has_tags:
        block.append(indent + "tags: []")

    return fm[:m_start] + block + fm[m_end:]


def process_file(path, neighbors):
    """Return (changed, new_text, has_tags_after). `neighbors` = symmetric sister set."""
    text = path.read_text(encoding="utf-8")
    fm_lines, body, ok = split_frontmatter(text)
    if not ok:
        return False, text, False
    fm_lines = collapse_block_lists(fm_lines)   # R6: canonicalize harness block-style -> inline
    new_fm = transform_fm(fm_lines, neighbors)
    new_text = "---\n" + "\n".join(new_fm) + "\n---\n" + body
    has_tags = any(re.match(r"^\s*tags:\s*\[.+\]", l) for l in new_fm)  # non-empty tags
    return (new_text != text), new_text, has_tags


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true", help="write changes (default: dry-run)")
    ap.add_argument("--dry-run", action="store_true", help="print unified diffs, no writes")
    ap.add_argument("--report", action="store_true", help="tags/links status per file")
    ap.add_argument("--paths", nargs="*", help="specific memory files (default: all)")
    ap.add_argument("--set-tags", help="JSON {filename: [tags]} -> set metadata.tags on each")
    args = ap.parse_args()

    mem_dir = _resolve_memory_dir()
    if mem_dir is None:
        print("ERROR: memory dir not resolvable (set $FOXML_MEMORY_DIR)", file=sys.stderr)
        return 2

    if args.set_tags:                       # judgment tag pass (separate mode)
        import json
        mapping = json.loads(Path(args.set_tags).read_text(encoding="utf-8"))
        applied = 0
        for fname, tags in mapping.items():
            if fname.startswith("_"):
                continue
            fp = mem_dir / fname
            if not fp.exists():
                print(f"  WARN: {fname} not found")
                continue
            did, new_text = set_tags_in_text(fp.read_text(encoding="utf-8"), tags)
            if did:
                applied += 1
                if args.apply:
                    fp.write_text(new_text, encoding="utf-8")
        print(f"{'APPLIED' if args.apply else 'DRY-RUN'}: set tags on {applied} files")
        return 0

    name_idx = build_name_index(mem_dir)
    all_mem = [f for f in sorted(mem_dir.glob("*.md")) if f.name != "MEMORY.md"]
    # Build the UNDIRECTED sister graph from every memory body. "Related" is mutual,
    # so auto-reciprocate: a body link A->B yields a sister edge on BOTH A and B.
    # Derived sister_specs are then symmetric by construction -> no one-way asymmetry
    # (closes CP-1 for the derived set; the guard still catches manual asymmetries).
    graph = {f.name: set() for f in all_mem}
    unresolved_by_file = {}
    for f in all_mem:
        _, body, ok = split_frontmatter(f.read_text(encoding="utf-8"))
        if not ok:
            continue
        sisters, unresolved = derive_sisters(body, name_idx)
        if unresolved:
            unresolved_by_file[f.name] = unresolved
        for s in sisters:
            graph.setdefault(f.name, set()).add(s)
            graph.setdefault(s, set()).add(f.name)

    files = [Path(p) for p in args.paths] if args.paths else all_mem
    changed = missing_tags = 0
    all_unresolved = []
    for f in files:
        if not f.exists():
            continue
        neighbors = sorted(graph.get(f.name, set()))
        did_change, new_text, has_tags = process_file(f, neighbors)
        unresolved = unresolved_by_file.get(f.name, [])
        if unresolved:
            all_unresolved.append((f.name, unresolved))
        if not has_tags:
            missing_tags += 1
        if did_change:
            changed += 1
        if args.report:
            flag = "" if has_tags else "  [NO TAGS]"
            unr = (" UNRESOLVED=" + ",".join(unresolved)) if unresolved else ""
            print(f"{'CHG' if did_change else ' = '} {f.name}{flag}{unr}")
            continue
        if did_change:
            if args.apply:
                f.write_text(new_text, encoding="utf-8")
            else:
                diff = difflib.unified_diff(
                    f.read_text(encoding="utf-8").splitlines(True),
                    new_text.splitlines(True),
                    fromfile=str(f.name), tofile=str(f.name) + " (new)",
                )
                sys.stdout.writelines(diff)

    print(f"\n{'APPLIED' if args.apply else 'DRY-RUN'}: {changed} changed / {len(files)} files; "
          f"{missing_tags} still need tags; {len(all_unresolved)} files with unresolved [[links]]")
    for name, unr in all_unresolved:
        print(f"  INFO dangling [[link]] in {name}: {unr} (forward-ref to an unwritten memory; non-fatal)")
    return 0   # dangling [[links]] are INFO (the memory system allows forward-refs); non-zero reserved for real errors (no mem dir -> 2)


if __name__ == "__main__":
    sys.exit(main())
