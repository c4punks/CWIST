#!/usr/bin/env python3
"""Compile-check the tutorials so they cannot silently drift from the API.

Nothing else in CI builds tutorials/ or the code in docs/tutorial/, and both
have repeatedly ended up calling functions that no longer exist. This gate:

  1. builds and links every tutorials/*/main.c against libcwist.a with the
     project CFLAGS (so -Werror applies when the caller passes it), and
  2. compiles every ```c block in docs/tutorial/cwist_tutorial.md with
     -fsyntax-only (the blocks are standalone files but not all have main()).

Nothing is executed: most tutorials start a server and never return.

Usage (normally via `make tutorials-check`):
    python3 scripts/ci/check_tutorials.py --cc cc --cflags "..." \\
        --lib libcwist.a --libs "..." [--out build-tutorials]
"""
import argparse
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
TUTORIAL_DOC = ROOT / "docs" / "tutorial" / "cwist_tutorial.md"
SNIPPET_RE = re.compile(r"^```c\n(.*?)^```", re.S | re.M)


def run(cmd):
    """Run a compiler command; return (ok, combined output)."""
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return proc.returncode == 0, (proc.stdout + proc.stderr).strip()


def build_tutorials(cc, cflags, lib, libs, out_dir):
    failures = []
    mains = sorted(ROOT.glob("tutorials/*/main.c"))
    for src in mains:
        name = src.parent.name
        exe = out_dir / name
        cmd = [cc, *cflags, "-o", str(exe), str(src.relative_to(ROOT)), lib, *libs]
        ok, output = run(cmd)
        print(f"{'ok  ' if ok else 'FAIL'} tutorials/{name}/main.c")
        if not ok:
            failures.append((f"tutorials/{name}/main.c", output))
    return len(mains), failures


def check_snippets(cc, cflags, work_dir):
    failures = []
    text = TUTORIAL_DOC.read_text(encoding="utf-8").replace("\r\n", "\n")
    # Snippets are compiled without -Werror and with unused-parameter warnings
    # off: example handlers routinely ignore req, and the point is to catch
    # calls and types that do not exist, not to lint prose examples.
    flags = [f for f in cflags if f != "-Werror"] + ["-Wno-unused-parameter", "-fsyntax-only"]
    blocks = list(SNIPPET_RE.finditer(text))
    for i, m in enumerate(blocks, 1):
        line = text.count("\n", 0, m.start()) + 1
        path = work_dir / f"snippet_{i:02d}.c"
        path.write_text(m.group(1), encoding="utf-8")
        ok, output = run([cc, *flags, str(path)])
        label = f"{TUTORIAL_DOC.relative_to(ROOT).as_posix()}:{line}"
        print(f"{'ok  ' if ok else 'FAIL'} {label}")
        if not ok:
            failures.append((label, output))
    return len(blocks), failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cc", required=True)
    ap.add_argument("--cflags", default="")
    ap.add_argument("--lib", required=True, help="path to libcwist.a")
    ap.add_argument("--libs", default="", help="link flags after the library")
    ap.add_argument("--out", default="build-tutorials", help="directory for tutorial binaries")
    args = ap.parse_args()

    cflags = shlex.split(args.cflags)
    libs = shlex.split(args.libs)
    out_dir = ROOT / args.out
    out_dir.mkdir(parents=True, exist_ok=True)

    n_tut, tut_fail = build_tutorials(args.cc, cflags, args.lib, libs, out_dir)
    with tempfile.TemporaryDirectory() as tmp:
        n_snip, snip_fail = check_snippets(args.cc, cflags, pathlib.Path(tmp))

    failures = tut_fail + snip_fail
    for label, output in failures:
        print(f"\n--- {label} ---\n{output}")
    print(f"\ntutorials: {n_tut - len(tut_fail)}/{n_tut} built, "
          f"tutorial snippets: {n_snip - len(snip_fail)}/{n_snip} compiled")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
