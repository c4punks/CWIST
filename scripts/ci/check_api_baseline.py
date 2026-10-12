#!/usr/bin/env python3
"""API stability gate: public headers must match docs/api/v4.0-api-baseline.txt.

Two checks:

  1. Drift. The baseline regenerated from the current headers must equal the
     committed file. Any public API change (a new function, a changed
     signature, a struct member added or moved) fails until the PR also
     commits the regenerated baseline (`make api-baseline`). That baseline
     diff is the PR's explicit acknowledgment of the change, and it is what
     review looks at.

  2. Guarantee. Once the guarantee ref exists (the LTS tag v4 by default, or
     --ref / CWIST_API_GUARANTEE_REF), every line in the guaranteed sections
     of that ref's baseline must still be present. Removing or changing a
     guaranteed function, signature, type, constant or struct member then
     fails even with the baseline regenerated; adding new API does not.
     Experimental sections are not checked. Before the ref exists the check
     is skipped with a note.

Usage: python3 scripts/ci/check_api_baseline.py [--ref REF] [repo_root]
Exit 0 = OK, 1 = gate failure.
"""
from __future__ import annotations

import argparse
import difflib
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from api_baseline import render  # noqa: E402

BASELINE = "docs/api/v4.0-api-baseline.txt"
DEFAULT_REF = "v4"


def guaranteed_lines(text: str) -> dict[str, set[str]]:
    """Map each non-experimental section title to its entries."""
    sections: dict[str, set[str]] = {}
    current = None
    for line in text.splitlines():
        m = re.match(r"^== (.+?) \(\d+\) ==$", line)
        if m:
            title = m.group(1)
            current = None if title.startswith("experimental") else title
            if current:
                sections.setdefault(current, set())
            continue
        if line.startswith("#"):
            continue
        if current and line:
            sections[current].add(line)
    return sections


def removed_guaranteed(old: str, new: str) -> list[str]:
    old_s, new_s = guaranteed_lines(old), guaranteed_lines(new)
    new_all = set().union(*new_s.values()) if new_s else set()
    out = []
    for title, lines in sorted(old_s.items()):
        for line in sorted(lines - new_all):
            out.append(f"{title}: {line}")
    return out


def ref_baseline(root: Path, ref: str) -> str | None:
    try:
        return subprocess.check_output(["git", "show", f"{ref}:{BASELINE}"], cwd=root,
                                       text=True, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        return None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ref", default=os.environ.get("CWIST_API_GUARANTEE_REF", DEFAULT_REF))
    ap.add_argument("root", nargs="?", default=".")
    args = ap.parse_args(argv)
    root = Path(args.root).resolve()

    current = render(root)
    committed = (root / BASELINE).read_text(encoding="utf-8")
    failed = False

    if current != committed:
        failed = True
        diff = list(difflib.unified_diff(committed.splitlines(), current.splitlines(),
                                         f"{BASELINE} (committed)", "headers (current)",
                                         lineterm="", n=0))
        print(f"FAIL: public headers no longer match {BASELINE}.")
        print("If the change is intended, run `make api-baseline` and commit the result;")
        print("the baseline diff is the PR's record of the API change.\n")
        for line in diff[:200]:
            print(line)
        if len(diff) > 200:
            print(f"... {len(diff) - 200} more diff lines")
    else:
        print(f"OK: public headers match {BASELINE}.")

    old = ref_baseline(root, args.ref)
    if old is None:
        print(f"note: {args.ref} has no {BASELINE}; guarantee check skipped until it does.")
    else:
        removed = removed_guaranteed(old, current)
        if removed:
            failed = True
            print(f"\nFAIL: {len(removed)} guaranteed entr{'y' if len(removed) == 1 else 'ies'} "
                  f"from {args.ref} removed or changed (the v4.0 API guarantee):")
            for line in removed[:200]:
                print(f"  {line}")
        else:
            print(f"OK: every guaranteed entry from {args.ref} is still present.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
