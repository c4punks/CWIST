#!/usr/bin/env python3
"""
Produce a sqlite-style single-file distribution of CWIST.

Outputs:
  <outdir>/cwist.c   — all CWIST translation units concatenated, with
                        internal #includes inlined once and then suppressed
  <outdir>/cwist.h   — umbrella public header

Usage (called by `make amalgamation`):
  python3 scripts/amalgamate.py \
      --srcs src/core/sstring/sstring.c src/net/http/http.c ... \
      --include-dirs include lib lib/libttak/include lib/cjson \
                     lib/sqlite3 lib/uriparser/include lib/cnats/src \
                     lib/boringssl/include lib/lsquic/include \
                     lib/multipart-parser-c \
      --public-include-root include/cwist \
      --out dist/cwist-amalgamation
"""

import argparse
import os
import re
import sys

# Matches both angle-bracket and quoted #include lines.
_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])(.*?)[>"]')


def resolve(header: str, include_dirs: list[str], relative_to: str | None) -> str | None:
    """Return the absolute path for an #include reference, or None."""
    if relative_to:
        candidate = os.path.normpath(os.path.join(relative_to, header))
        if os.path.isfile(candidate):
            return candidate
    for d in include_dirs:
        candidate = os.path.normpath(os.path.join(d, header))
        if os.path.isfile(candidate):
            return candidate
    return None


def is_internal(path: str, root: str) -> bool:
    """True when path lives under the repo root (should be inlined)."""
    try:
        os.path.relpath(path, root)
        return os.path.abspath(path).startswith(os.path.abspath(root) + os.sep) or \
               os.path.abspath(path) == os.path.abspath(root)
    except ValueError:
        return False


def expand(
    src: str,
    include_dirs: list[str],
    repo_root: str,
    seen: set[str],
    external_includes: list[str],
    seen_external: set[str],
    depth: int = 0,
) -> list[str]:
    """
    Return the lines of `src` with internal #includes expanded recursively
    (once each), external #includes collected, and the file marker emitted.
    """
    abs_src = os.path.abspath(src)
    if abs_src in seen:
        return []
    seen.add(abs_src)

    lines = []
    src_dir = os.path.dirname(abs_src)

    try:
        raw = open(abs_src, encoding="utf-8", errors="replace").read()
    except OSError as e:
        lines.append(f"/* amalgamate: could not read {src}: {e} */\n")
        return lines

    lines.append(f"\n/* ---- {os.path.relpath(abs_src, repo_root)} ---- */\n")

    in_guard = False  # track a simple top-level include guard to strip it
    guard_macro = None
    file_lines = raw.splitlines(keepends=True)
    i = 0
    while i < len(file_lines):
        line = file_lines[i]
        m = _INCLUDE_RE.match(line)
        if m:
            bracket, header = m.group(1), m.group(2)
            relative_to = src_dir if bracket == '"' else None
            resolved = resolve(header, include_dirs, relative_to)
            if resolved and is_internal(resolved, repo_root):
                # Inline the header once.
                sub = expand(resolved, include_dirs, repo_root, seen,
                             external_includes, seen_external, depth + 1)
                lines.extend(sub)
            else:
                # External include — collect once.
                norm = f"#include <{header}>\n" if bracket == "<" else f'#include "{header}"\n'
                if norm not in seen_external:
                    seen_external.add(norm)
                    external_includes.append(norm)
            i += 1
            continue

        # Strip simple include guards from header files being inlined.
        if depth > 0:
            stripped = line.strip()
            if not in_guard and stripped.startswith("#ifndef ") and i == 0:
                guard_macro = stripped.split()[1]
                in_guard = True
                i += 1
                continue
            if in_guard and i == 1 and stripped.startswith(f"#define {guard_macro}"):
                i += 1
                continue
            if in_guard and stripped == f"#endif" and i == len(file_lines) - 1:
                i += 1
                continue
            if in_guard and stripped == f"#endif /* {guard_macro} */":
                i += 1
                continue
            if in_guard and stripped in (
                f"#endif // {guard_macro}",
                f"#endif  // {guard_macro}",
            ):
                i += 1
                continue

        lines.append(line)
        i += 1

    return lines


def public_headers(public_root: str) -> list[str]:
    """Return all .h files under public_root, sorted by path."""
    result = []
    for dirpath, _, filenames in os.walk(public_root):
        for f in sorted(filenames):
            if f.endswith(".h"):
                result.append(os.path.join(dirpath, f))
    return result


def build_umbrella(public_root: str, out_h: str) -> None:
    """Write the cwist.h umbrella that #includes every public header."""
    root_abs = os.path.abspath(public_root)
    out_dir = os.path.dirname(out_h)
    lines = [
        "/* cwist.h — generated umbrella header; do not edit */\n",
        "#ifndef CWIST_AMALGAMATION_H\n",
        "#define CWIST_AMALGAMATION_H\n",
        "\n",
    ]
    for h in public_headers(public_root):
        rel = os.path.relpath(h, root_abs)
        # Emit as #include <cwist/rel>
        lines.append(f'#include <cwist/{rel.replace(os.sep, "/")}>\n')
    lines += ["\n", "#endif /* CWIST_AMALGAMATION_H */\n"]
    os.makedirs(out_dir, exist_ok=True)
    with open(out_h, "w", encoding="utf-8") as f:
        f.writelines(lines)
    print(f"  wrote {out_h} ({len(public_headers(public_root))} public headers)")


def main() -> None:
    ap = argparse.ArgumentParser(description="Produce CWIST amalgamation")
    ap.add_argument("--srcs", nargs="+", required=True, help=".c files in link order")
    ap.add_argument("--include-dirs", nargs="+", default=[], dest="include_dirs")
    ap.add_argument("--public-include-root", required=True, dest="public_root",
                    help="Directory that contains the public cwist/ headers (include/cwist)")
    ap.add_argument("--out", required=True, help="Output directory")
    ap.add_argument("--repo-root", default=".", dest="repo_root",
                    help="Repository root (used to decide what is internal)")
    args = ap.parse_args()

    out_c = os.path.join(args.out, "cwist.c")
    out_h = os.path.join(args.out, "cwist.h")
    os.makedirs(args.out, exist_ok=True)

    repo_root = os.path.abspath(args.repo_root)
    include_dirs = [os.path.abspath(d) for d in args.include_dirs]

    # Phase 1 — public umbrella header.
    build_umbrella(os.path.join(args.public_root, ".."), out_h)

    # Phase 2 — amalgamated .c.
    seen: set[str] = set()
    external_includes: list[str] = []
    seen_external: set[str] = set()
    body_lines: list[str] = []

    for src in args.srcs:
        body_lines.extend(
            expand(src, include_dirs, repo_root, seen,
                   external_includes, seen_external)
        )

    # Also set public_root correctly for the expand phase
    public_root = os.path.abspath(args.public_root)

    banner = [
        "/* cwist.c — generated amalgamation; do not edit\n",
        " *\n",
        " * To build:\n",
        " *   cc -std=c17 -I<dir-containing-cwist.h> cwist.c -o cwist.o\n",
        " * Link against the vendored dependencies listed in the CWIST README.\n",
        " */\n",
        '\n#include "cwist.h"\n\n',
    ]

    with open(out_c, "w", encoding="utf-8") as f:
        f.writelines(banner)
        f.writelines(external_includes)
        f.write("\n")
        f.writelines(body_lines)

    src_lines = sum(1 for _ in open(out_c, encoding="utf-8"))
    print(f"  wrote {out_c} ({src_lines} lines, {len(args.srcs)} translation units)")


if __name__ == "__main__":
    main()
