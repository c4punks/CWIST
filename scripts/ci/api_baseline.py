#!/usr/bin/env python3
"""Generate the CWIST public API baseline (docs/api/v4.0-api-baseline.txt).

The baseline records the C public surface of include/cwist/**:

  * names: functions, struct/union/enum names, typedefs, enum constants,
    extern globals;
  * function signatures, with parameter names dropped so a rename is not an
    API change;
  * struct, union and enum layouts: every member in declaration order (and
    every preprocessor conditional around it), and every enumerator with its
    explicit value.

WebTransport symbols are experimental until v4.1 (ROADMAP "API surface
decisions") and are recorded in separate "experimental" sections outside the
guarantee.

Output is sorted (layouts keep member order) and independent of header read
order, so diffs review cleanly.

Usage: python3 scripts/ci/api_baseline.py [repo_root]   (writes to stdout)
       make api-baseline                                 (rewrites the file)
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

EXPERIMENTAL = re.compile(r"webtransport", re.I)

# Words that end a parameter type rather than name it: `unsigned int` keeps
# its `int`.
TYPE_WORDS = {"int", "char", "long", "short", "double", "float", "void", "signed",
              "unsigned", "bool", "_Bool", "const", "volatile", "restrict"}
TYPE_PREFIXES = {"", "const", "struct", "union", "enum", "unsigned", "signed", "volatile"}


def strip_comments(text: str) -> str:
    text = re.sub(r"\\\n", " ", text)  # join continuation lines first
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def without_directives(text: str) -> str:
    """Replace each preprocessor line with ';' so it separates declarations.

    Without this, a declaration right after `#include`, `#define` or `#endif`
    has no ';' or '}' before it and the declaration matchers skip it.
    """
    return re.sub(r"^[ \t]*#[^\n]*", ";", text, flags=re.M)


def without_function_bodies(text: str) -> str:
    """Replace the bodies of inline function definitions with ';'.

    A call inside a `static inline` body (`return cwist_x(a, b);`) otherwise
    reads as a declaration of cwist_x.
    """
    out, i = [], 0
    for m in re.finditer(r"\)\s*\{", text):
        if m.start() < i:
            continue
        close = match_brace(text, m.end() - 1)
        if close < 0:
            break
        out.append(text[i:m.start() + 1])
        out.append(";")
        i = close + 1
    out.append(text[i:])
    return "".join(out)


def norm_type(text: str) -> str:
    text = re.sub(r"\s+", " ", text).strip()
    text = re.sub(r"\s*\*\s*", "*", text)
    text = re.sub(r"(?<=[\w)])\*", " *", text)
    text = re.sub(r"\s*,\s*", ", ", text)
    text = re.sub(r"\(\s+", "(", text)
    return re.sub(r"\s+\)", ")", text)


def split_top(text: str, sep: str) -> list[str]:
    parts, depth, cur = [], 0, []
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def strip_param_name(param: str) -> str:
    p = re.sub(r"\s+", " ", param).strip()
    if p in ("", "void", "..."):
        return p
    # Function pointer parameter: `ret (*name)(args)` -> `ret (*)(args)`.
    fp = re.sub(r"\(\s*\*\s*\w+\s*\)", "(*)", p, count=1)
    if fp != p:
        return norm_type(fp)
    arrays = ""
    m = re.search(r"(\s*\[[^\]]*\])+\s*$", p)
    if m:
        arrays = "[]" * m.group(0).count("[")
        p = p[:m.start()]
    m = re.match(r"^(.*?[\s*])(\w+)$", p)
    if m and m.group(2) not in TYPE_WORDS and m.group(1).strip() not in TYPE_PREFIXES:
        p = m.group(1)
    return norm_type(p) + arrays


def signature(ret: str, name: str, params: str) -> str:
    ret = re.sub(r"\b(extern|static|inline|__inline__|CWIST_WASM_EXPORT)\b", " ", ret)
    args = ", ".join(strip_param_name(p) for p in split_top(params, ","))
    return f"{name}: {norm_type(ret)} ({args})"


def match_brace(text: str, open_idx: int) -> int:
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


def layout_entries(body: str, kind: str) -> list[str]:
    """Members of a struct/union body, or enumerators of an enum body, in order.

    Preprocessor conditionals inside the body are kept as entries, since a
    member under `#ifdef` changes the layout per build.
    """
    sep = "," if kind == "enum" else ";"
    out: list[str] = []
    cur: list[str] = []

    def flush() -> None:
        for piece in split_top(" ".join(cur), sep):
            piece = norm_type(piece)
            if piece:
                out.append(piece)
        cur.clear()

    for line in body.split("\n"):
        if re.match(r"^\s*#", line):
            flush()
            out.append(re.sub(r"\s+", " ", line.strip()))
        else:
            cur.append(line)
    flush()
    return out


def collect(root: Path) -> dict[str, set[str] | dict[str, list[str]]]:
    headers = subprocess.check_output(["git", "ls-files", "include/cwist"], text=True,
                                      cwd=root).split()
    functions, typedefs, types, constants, globals_ = set(), set(), set(), set(), set()
    signatures: set[str] = set()
    inline_signatures: set[str] = set()
    layouts: dict[str, list[str]] = {}

    for path in headers:
        if not path.endswith(".h"):
            continue
        raw = strip_comments((root / path).read_text(encoding="utf-8"))
        buf = without_function_bodies(without_directives(raw))

        for m in re.finditer(r"typedef\s+enum\s*(\w*)\s*\{(.*?)\}\s*(\w*)\s*;", buf, flags=re.S):
            tag, body, name = m.group(1), m.group(2), m.group(3)
            if tag:
                types.add(tag)
            if name:
                typedefs.add(name)
            constants.update(re.findall(r"\b(CWIST_[A-Z0-9_]+)\b", body))
        for m in re.finditer(r"(?<!typedef\s)enum\s+(\w+)\s*\{", buf):
            types.add(m.group(1))
        for m in re.finditer(r"(?<!typedef\s)enum\s*\{(.*?)\}\s*;", buf, flags=re.S):
            constants.update(re.findall(r"\b(CWIST_[A-Z0-9_]+)\b", m.group(1)))

        for m in re.finditer(r"typedef\s+(?:struct|union)\s*(\w*)\s*\{.*?}\s*(\w+)\s*;", buf,
                             flags=re.S):
            if m.group(1):
                types.add(m.group(1))
            typedefs.add(m.group(2))
        for m in re.finditer(r"(?<!typedef\s)(?:struct|union)\s+(cwist_\w+)\s*[;{]", buf):
            types.add(m.group(1))
        for m in re.finditer(r"typedef\s+(?:struct|union|enum)\s+(\w+)\s+(\w+)\s*;", buf):
            types.add(m.group(1))
            typedefs.add(m.group(2))
        for m in re.finditer(r"typedef\s+(?!struct\b)(?!union\b)(?!enum\b)([^;]+?)\s+(\w+)\s*;",
                             buf):
            typedefs.add(m.group(2))
        for m in re.finditer(r"typedef\s+[^;]*?\(\s*\*\s*(\w+)\s*\)\s*\([^;]*\)\s*;", buf):
            typedefs.add(m.group(1))

        # Function declarations. The preceding ';' or '}' is a lookbehind, not
        # part of the match, so back-to-back declarations all anchor.
        for m in re.finditer(r"(?:^|(?<=[;}]))\s*([\w\s\*]+?)\b(cwist_\w+)\s*\(([^;{}]*)\)\s*;",
                             buf):
            ret, name, params = m.group(1), m.group(2), m.group(3)
            if name in typedefs or re.search(r"\btypedef\b", ret):
                continue
            if re.search(r"\binline\b", ret):
                # A header-defined helper: source API, but no library symbol
                # (check_inline_exports.py decides which get one).
                inline_signatures.add(signature(ret, name, params))
                continue
            functions.add(name)
            signatures.add(signature(ret, name, params))

        for m in re.finditer(r"extern\s+[^;]*?\b(cwist_\w+)\s*(?:\[[^]]*\])?\s*;", buf):
            globals_.add(m.group(1))

        # Layouts, from the text that still has its directives.
        for m in re.finditer(r"\b(typedef\s+)?(struct|union|enum)\s*(\w*)\s*\{", raw):
            # Skip bodies nested inside another layout (counted with their parent).
            before = raw[:m.start()]
            if before.count("{") > before.count("}"):
                continue
            close = match_brace(raw, m.end() - 1)
            if close < 0:
                continue
            kind, tag = m.group(2), m.group(3)
            tail = re.match(r"\s*(\w*)\s*;", raw[close + 1:])
            name = (tail.group(1) if m.group(1) and tail and tail.group(1) else "") or tag
            if not name:
                continue
            layouts[f"{kind} {name}"] = layout_entries(raw[m.end():close], kind)

    # Function-pointer typedefs look like declarations to the matcher above.
    functions -= typedefs
    signatures = {s for s in signatures if s.split(":", 1)[0] in functions}
    inline_signatures = {s for s in inline_signatures if s.split(":", 1)[0] not in functions}
    return {"functions": functions, "signatures": signatures,
            "inline_signatures": inline_signatures, "types": types,
            "typedefs": typedefs, "constants": constants, "globals": globals_,
            "layouts": layouts}


def split_experimental(items):
    exp = {it for it in items if EXPERIMENTAL.search(it.split(":", 1)[0])}
    return items - exp, exp


def render(root: Path) -> str:
    c = collect(root)
    out: list[str] = []

    def section(title: str, items) -> None:
        out.append(f"\n== {title} ({len(items)}) ==")
        out.extend(sorted(items))

    def layout_section(title: str, layouts: dict[str, list[str]]) -> None:
        out.append(f"\n== {title} ({len(layouts)}) ==")
        for name in sorted(layouts):
            for i, entry in enumerate(layouts[name]):
                out.append(f"{name} [{i}] {entry}")

    funcs, exp_funcs = split_experimental(c["functions"])
    sigs, exp_sigs = split_experimental(c["signatures"])
    inline_sigs, exp_inline_sigs = split_experimental(c["inline_signatures"])
    types, exp_types = split_experimental(c["types"])
    typedefs, exp_typedefs = split_experimental(c["typedefs"])
    consts, exp_consts = split_experimental(c["constants"])
    globs, exp_globs = split_experimental(c["globals"])
    layouts = {k: v for k, v in c["layouts"].items() if not EXPERIMENTAL.search(k)}
    exp_layouts = {k: v for k, v in c["layouts"].items() if EXPERIMENTAL.search(k)}

    out.append("# CWIST v4.0 public API baseline")
    out.append("# Regenerated by `make api-baseline`; do not edit by hand.")
    out.append("# Covers include/cwist/** public headers: names, function signatures")
    out.append("# (parameter names dropped) and struct/union/enum layouts in member order.")
    out.append("# `make api-check` (CI) fails when the headers no longer match this file.")
    section("functions", funcs)
    section("function signatures", sigs)
    section("inline function signatures", inline_sigs)
    section("types (struct/union/enum names)", types)
    section("typedefs", typedefs)
    section("enum constants", consts)
    section("globals (extern)", globs)
    layout_section("layouts", layouts)
    out.append("\n# Experimental: outside the v4.0 guarantee (WebTransport, until v4.1).")
    section("experimental functions", exp_funcs)
    section("experimental function signatures", exp_sigs)
    section("experimental inline function signatures", exp_inline_sigs)
    section("experimental types (struct/union/enum names)", exp_types)
    section("experimental typedefs", exp_typedefs)
    section("experimental enum constants", exp_consts)
    section("experimental globals (extern)", exp_globs)
    layout_section("experimental layouts", exp_layouts)
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    sys.stdout.write(render(root))
