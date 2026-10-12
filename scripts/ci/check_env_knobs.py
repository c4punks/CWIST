#!/usr/bin/env python3
"""Runtime knob gate: every CWIST_* variable src/ reads is in docs/runtime-config.md.

Three checks:

  1. Coverage. The set of names read with getenv("CWIST_...") under src/
     must equal the set of variables in the docs/runtime-config.md tables.
     A new knob fails until it is documented; a documented knob nothing reads
     any more fails until its row is removed.

  2. Mirror. docs/ko/runtime-config.md lists the same variables with the
     same status, so the Korean page cannot fall behind.

  3. Guarantee. Once the guarantee ref exists (the LTS tag v4 by default, or
     --ref / CWIST_API_GUARANTEE_REF), every variable that ref's page marks
     `supported` must still be read by src/ and still be `supported`. Before
     the ref exists the check is skipped with a note.

Usage: python3 scripts/ci/check_env_knobs.py [--ref REF] [repo_root]
Exit 0 = OK, 1 = gate failure.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

DOC = "docs/runtime-config.md"
DOC_KO = "docs/ko/runtime-config.md"
DEFAULT_REF = "v4"
STATUSES = ("supported", "tuning", "diagnostic")

GETENV_RE = re.compile(r'getenv\(\s*"(CWIST_[A-Z0-9_]+)"\s*\)')
ROW_RE = re.compile(r"^\|\s*`(CWIST_[A-Z0-9_]+)`\s*\|\s*([a-z]+)\s*\|")


def read_knobs(src_root: Path) -> dict[str, list[str]]:
    """Map each CWIST_* name read with getenv() to the files that read it."""
    found: dict[str, list[str]] = {}
    for path in sorted(src_root.rglob("*")):
        if path.suffix not in (".c", ".h") or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for name in GETENV_RE.findall(text):
            found.setdefault(name, []).append(str(path))
    return found


def parse_doc(text: str) -> tuple[dict[str, str], list[str]]:
    """Return {name: status} from the doc's tables, plus format errors."""
    rows: dict[str, str] = {}
    errors: list[str] = []
    for line in text.splitlines():
        m = ROW_RE.match(line)
        if not m:
            continue
        name, status = m.groups()
        if status not in STATUSES:
            errors.append(f"{name}: unknown status '{status}' (use {', '.join(STATUSES)})")
        if name in rows:
            errors.append(f"{name}: listed twice")
        rows[name] = status
    return rows, errors


def doc_at_ref(root: Path, ref: str, path: str) -> str | None:
    """The doc as committed at `ref`, or None when the ref or file is missing."""
    try:
        return subprocess.run(
            ["git", "-C", str(root), "show", f"{ref}:{path}"],
            check=True, capture_output=True, text=True,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def check(root: Path, ref: str | None) -> list[str]:
    failures: list[str] = []
    read = read_knobs(root / "src")

    doc_path = root / DOC
    if not doc_path.is_file():
        return [f"{DOC} is missing"]
    doc, errors = parse_doc(doc_path.read_text(encoding="utf-8"))
    failures += [f"{DOC}: {e}" for e in errors]

    for name in sorted(set(read) - set(doc)):
        failures.append(f"{name} is read by {read[name][0]} but not documented in {DOC}")
    for name in sorted(set(doc) - set(read)):
        failures.append(f"{name} is documented in {DOC} but nothing in src/ reads it")

    ko_path = root / DOC_KO
    if not ko_path.is_file():
        failures.append(f"{DOC_KO} is missing (Korean mirror of {DOC})")
    else:
        ko, ko_errors = parse_doc(ko_path.read_text(encoding="utf-8"))
        failures += [f"{DOC_KO}: {e}" for e in ko_errors]
        for name in sorted(set(doc) - set(ko)):
            failures.append(f"{name} is missing from {DOC_KO}")
        for name in sorted(set(ko) - set(doc)):
            failures.append(f"{name} is in {DOC_KO} but not in {DOC}")
        for name in sorted(set(doc) & set(ko)):
            if doc[name] != ko[name]:
                failures.append(f"{name}: status '{doc[name]}' in {DOC} but '{ko[name]}' in {DOC_KO}")

    if ref:
        old_text = doc_at_ref(root, ref, DOC)
        if old_text is None:
            print(f"note: {ref} has no {DOC}; guarantee check skipped until it does.")
        else:
            old, _ = parse_doc(old_text)
            for name, status in sorted(old.items()):
                if status != "supported":
                    continue
                if name not in read:
                    failures.append(f"{name} is supported in {ref} but src/ no longer reads it")
                elif doc.get(name) != "supported":
                    failures.append(
                        f"{name} is supported in {ref} but now '{doc.get(name, 'undocumented')}'"
                    )
    return failures


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ref", default=os.environ.get("CWIST_API_GUARANTEE_REF", DEFAULT_REF))
    ap.add_argument("root", nargs="?", default=str(Path(__file__).resolve().parents[2]))
    args = ap.parse_args(argv)
    failures = check(Path(args.root), args.ref or None)
    if failures:
        print(f"FAIL: runtime knobs and {DOC} disagree:")
        for f in failures:
            print(f"  {f}")
        return 1
    print(f"OK: every CWIST_* variable read by src/ is documented in {DOC}.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
