#!/usr/bin/env python3
"""Generate the CWIST amalgamated single-file distribution (issue #429).

The Makefile's `amalgamation` target invokes this script with the final,
knob-expanded SRCS list and the -D defines from CFLAGS.  The output is a
self-contained bundle under the output directory:

    cwist.c              every enabled CWIST translation unit pasted into one
                         file, in deterministic (Makefile SRCS) order
    include/cwist/**     the public headers
    include/cwist.h      umbrella header including every public header
    include/cjson/, include/ttak/, include/openssl/, include/sqlite3.h
                         vendored headers the public API references (same
                         staging as `make fat-dist`)
    include/cwist-priv/  private headers the pasted bodies reference, keyed
                         by repo-relative path
    include/cwist-vendor/lsquic|cnats|usrsctp/
                         feature-gated vendored headers (only when the
                         matching sources are part of the build)

Quoted includes inside the pasted bodies are rewritten to the staged
location so the bundle compiles with a single -I<out>/include.
"""

import argparse
import os
import re
import shutil
import sys

INCLUDE_RE = re.compile(
    r'^(?P<indent>\s*#\s*include\s*)(?P<open>["<])(?P<path>[^">]+)[">](?P<rest>.*)$'
)

# Vendored headers that sources pull in through angle includes resolved by
# -I flags in the normal build.  They are staged under include/cwist-vendor/
# so the single -I of the bundle stays free of generically named headers
# (status.h, version.h, ...) at its root.
VENDORED_ANGLE = {
    "lsquic.h": "cwist-vendor/lsquic/lsquic.h",
    "lsquic_types.h": "cwist-vendor/lsquic/lsquic_types.h",
    "lsquic_wt.h": "cwist-vendor/lsquic/lsquic_wt.h",
    "lsxpack_header.h": "cwist-vendor/lsquic/lsxpack_header.h",
    "nats.h": "cwist-vendor/cnats/nats.h",
    "usrsctp.h": "cwist-vendor/usrsctp/usrsctp.h",
}

# Repo-relative prefix -> staged include prefix.  A quoted include that
# resolves to a file under one of these prefixes is rewritten to the mapped
# angle include; anything else inside the repo lands under cwist-priv/.
REWRITE_PREFIXES = [
    ("include/", ""),
    ("lib/libttak/include/", ""),
    ("lib/sqlite3/", ""),
    ("lib/lsquic/include/", "cwist-vendor/lsquic/"),
    ("lib/cnats/src/", "cwist-vendor/cnats/"),
    ("lib/usrsctp/usrsctplib/", "cwist-vendor/usrsctp/"),
    ("src/", "cwist-priv/"),
]


# Repo include directories from the Makefile's INCLUDE_PATHS, used as
# fallback when a quoted include does not resolve relative to its source
# file (e.g. src/net/http/multipart.c finds multipart_parser.h through
# -Ilib/multipart-parser-c in the normal build).
FALLBACK_INCLUDE_DIRS = [
    "lib",
    "lib/libttak/include",
    "lib/cjson",
    "lib/sqlite3",
    "lib/uriparser/include",
    "lib/cnats/src",
    "lib/boringssl/include",
    "lib/lsquic/include",
    "lib/multipart-parser-c",
]


def map_repo_header(relpath):
    for prefix, staged in REWRITE_PREFIXES:
        if relpath.startswith(prefix):
            return staged + relpath[len(prefix):]
    return "cwist-priv/" + relpath


def process_source(root, relpath):
    """Return the text of relpath with includes rewritten for the bundle.

    Used for both the pasted .c bodies and the staged private/vendored
    headers; relpath is always the original repo-relative path.
    """
    body_lines = []
    with open(os.path.join(root, relpath), encoding="utf-8") as f:
        for line in f:
            m = INCLUDE_RE.match(line)
            if m and m.group("open") == '"':
                resolved = os.path.normpath(
                    os.path.join(os.path.dirname(relpath), m.group("path"))
                )
                if resolved.startswith("..") or not os.path.isfile(
                    os.path.join(root, resolved)
                ):
                    resolved = None
                    for inc_dir in FALLBACK_INCLUDE_DIRS:
                        candidate = os.path.normpath(
                            os.path.join(inc_dir, m.group("path"))
                        )
                        if os.path.isfile(os.path.join(root, candidate)):
                            resolved = candidate
                            break
                if resolved is not None:
                    line = '%s<%s>%s\n' % (
                        m.group("indent"),
                        map_repo_header(resolved),
                        m.group("rest"),
                    )
            elif m and m.group("path") in VENDORED_ANGLE:
                line = '%s<%s>%s\n' % (
                    m.group("indent"),
                    VENDORED_ANGLE[m.group("path")],
                    m.group("rest"),
                )
            body_lines.append(line)
    return "".join(body_lines)


def copy_tree(root, src, dst):
    src_abs = os.path.join(root, src)
    if not os.path.isdir(src_abs):
        sys.exit("gen_amalgamation: missing directory %s (submodule not initialized?)" % src)
    shutil.copytree(src_abs, dst)


def copy_file(root, src, dst_dir):
    src_abs = os.path.join(root, src)
    if not os.path.isfile(src_abs):
        sys.exit("gen_amalgamation: missing file %s" % src)
    os.makedirs(dst_dir, exist_ok=True)
    shutil.copy2(src_abs, dst_dir)


# sqlite3.c carries the definition of sqlite3_version inside its embedded
# copy of the sqlite3.h section (guarded by SQLITE3_H).  In the unity build
# the public headers have already included the staged sqlite3.h, so that
# section is skipped and the symbol would be left undefined; provide it
# only in that case.
SQLITE_VERSION_COMPAT = """\
#ifdef SQLITE3_H
/* sqlite3.h was already included via the public headers; sqlite3.c's
 * embedded header section (which defines sqlite3_version) is skipped. */
const char sqlite3_version[] = SQLITE_VERSION;
#endif
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=".", help="repository root")
    ap.add_argument("--out", required=True, help="output bundle directory")
    ap.add_argument("--srcs", required=True, help="space-separated source list")
    ap.add_argument("--defines", default="", help="space-separated -D flags")
    ap.add_argument("--knobs", default="", help="human-readable knob summary")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    out = os.path.abspath(args.out)
    srcs = args.srcs.split()
    if not srcs:
        sys.exit("gen_amalgamation: empty source list")

    for src in srcs:
        if not os.path.isfile(os.path.join(root, src)):
            sys.exit("gen_amalgamation: missing source %s" % src)

    if os.path.isdir(out):
        shutil.rmtree(out)
    inc = os.path.join(out, "include")
    os.makedirs(inc)

    # --- public + vendored headers (same layout as `make fat-dist`) --------
    copy_tree(root, "include/cwist", os.path.join(inc, "cwist"))
    os.makedirs(os.path.join(inc, "cjson"))
    copy_file(root, "lib/cjson/cJSON.h", os.path.join(inc, "cjson"))
    copy_tree(root, "lib/libttak/include/ttak", os.path.join(inc, "ttak"))
    copy_tree(root, "lib/boringssl/include/openssl", os.path.join(inc, "openssl"))
    copy_file(root, "lib/sqlite3/sqlite3.h", inc)
    copy_file(root, "lib/sqlite3/sqlite3ext.h", inc)

    # --- private headers, keyed by repo-relative path under cwist-priv/ ----
    private_roots = ["src", "lib/libttak/src", "lib/libttak/internal",
                     "lib/multipart-parser-c"]
    for base in private_roots:
        for dirpath, _dirnames, filenames in os.walk(os.path.join(root, base)):
            for name in filenames:
                if not name.endswith(".h"):
                    continue
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, root)
                dst = os.path.join(inc, map_repo_header(rel))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "w", encoding="utf-8") as f:
                    f.write(process_source(root, rel))

    # --- feature-gated vendored headers ------------------------------------
    if any(s.startswith("src/net/http/http3") for s in srcs):
        for name in sorted(os.listdir(os.path.join(root, "lib/lsquic/include"))):
            if name.endswith(".h"):
                rel = "lib/lsquic/include/" + name
                dst = os.path.join(inc, "cwist-vendor/lsquic", name)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "w", encoding="utf-8") as f:
                    f.write(process_source(root, rel))
    if any(s == "src/net/nats/cwist_nats.c" for s in srcs):
        for name in sorted(os.listdir(os.path.join(root, "lib/cnats/src"))):
            if name.endswith(".h"):
                rel = "lib/cnats/src/" + name
                dst = os.path.join(inc, "cwist-vendor/cnats", name)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "w", encoding="utf-8") as f:
                    f.write(process_source(root, rel))
    if any(s.startswith("src/net/webrtc/") for s in srcs):
        rel = "lib/usrsctp/usrsctplib/usrsctp.h"
        dst = os.path.join(inc, "cwist-vendor/usrsctp", "usrsctp.h")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w", encoding="utf-8") as f:
            f.write(process_source(root, rel))

    # --- umbrella header ----------------------------------------------------
    public_headers = []
    for dirpath, _dirnames, filenames in os.walk(os.path.join(root, "include/cwist")):
        for name in filenames:
            if name.endswith(".h"):
                rel = os.path.relpath(os.path.join(dirpath, name),
                                      os.path.join(root, "include"))
                public_headers.append(rel)
    public_headers.sort()
    with open(os.path.join(inc, "cwist.h"), "w", encoding="utf-8") as f:
        f.write("/* Generated by scripts/gen_amalgamation.py -- DO NOT EDIT.\n")
        f.write(" * Umbrella header for the amalgamated CWIST distribution.\n")
        f.write(" * Knobs: %s\n" % (args.knobs or "(default)"))
        f.write(" */\n")
        f.write("#ifndef CWIST_AMALGAMATION_UMBRELLA_H\n")
        f.write("#define CWIST_AMALGAMATION_UMBRELLA_H\n\n")
        for header in public_headers:
            f.write("#include <%s>\n" % header)
        f.write("\n#endif /* CWIST_AMALGAMATION_UMBRELLA_H */\n")

    # --- unity source --------------------------------------------------------
    lines = []
    lines.append("/* Generated by scripts/gen_amalgamation.py -- DO NOT EDIT.\n")
    lines.append(" * CWIST amalgamated single-file distribution (issue #429).\n")
    lines.append(" * Knobs: %s\n" % (args.knobs or "(default)"))
    lines.append(" * Sources (%d, Makefile SRCS order):\n" % len(srcs))
    for src in srcs:
        lines.append(" *   %s\n" % src)
    lines.append(" *\n")
    lines.append(" * Build: gcc -O2 -std=gnu17 -Iinclude -c cwist.c\n")
    lines.append(" * and link with the vendored archives (see make fat-dist).\n")
    lines.append(" */\n")

    defines = args.defines.split()
    if defines:
        lines.append("\n/* Compile-time defines captured from the generating build. */\n")
        for define in defines:
            body = define[2:] if define.startswith("-D") else define
            if "=" in body:
                name, value = body.split("=", 1)
                lines.append("#ifndef %s\n#define %s %s\n#endif\n" % (name, name, value))
            else:
                lines.append("#ifndef %s\n#define %s 1\n#endif\n" % (body, body))

    for src in srcs:
        lines.append('\n#line 1 "%s"\n' % src)
        if src == "lib/sqlite3/sqlite3.c":
            lines.append(SQLITE_VERSION_COMPAT)
        lines.append(process_source(root, src))
        if not lines[-1].endswith("\n"):
            lines.append("\n")

    with open(os.path.join(out, "cwist.c"), "w", encoding="utf-8") as f:
        f.write("".join(lines))

    print("gen_amalgamation: %s/cwist.c from %d sources; headers in %s/include"
          % (out, len(srcs), out))


if __name__ == "__main__":
    main()
