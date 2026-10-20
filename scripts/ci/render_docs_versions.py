#!/usr/bin/env python3
"""Render site/versions.html, the index of per-tag Doxygen documentation trees.

Scans the assembled Pages site root for v* directories that contain a
Doxygen index.html, sorts them by version, and writes versions.html linking
each. Tags whose docs failed to generate are listed as unavailable.
"""
import re
import sys
from pathlib import Path


def version_key(tag: str):
    return [int(p) for p in re.findall(r"\d+", tag)]


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: render_docs_versions.py SITE_ROOT [expected tags...]", file=sys.stderr)
        return 2
    site = Path(sys.argv[1])
    expected = sorted(sys.argv[2:], key=version_key)
    available = [t for t in expected if (site / t / "index.html").is_file()]
    latest = available[-1] if available else None

    rows = []
    for tag in reversed(expected):
        if tag in available:
            label = f"{tag} (latest)" if tag == latest else tag
            rows.append(f'      <li><a href="{tag}/">{label}</a></li>')
        else:
            rows.append(f"      <li>{tag} (unavailable)</li>")
    items = "\n".join(rows)

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>CWIST documentation versions</title>
</head>
<body>
<h1>CWIST documentation versions</h1>
<p>Per-release Doxygen documentation, regenerated from each tag.</p>
<ul>
{items}
</ul>
<p><a href="./">Current development tree (main)</a></p>
</body>
</html>
"""
    (site / "versions.html").write_text(html, encoding="utf-8")
    print(f"versions.html: {len(available)}/{len(expected)} tags available")
    return 0


if __name__ == "__main__":
    sys.exit(main())
