#!/usr/bin/env python3
"""Fail CI when a public header gains a `static inline` helper nobody decided on.

Bindings generated from the public headers (bindgen for the Rust crate
planned in ROADMAP.md, v3.8 Phase 2) cannot call a `static inline`
function: it has no symbol in libcwist.a. Each such helper in include/cwist/
must therefore be either

  * EXPORTED: an out-of-line `<name>_extern()` wrapper is declared in a
    public header and defined in src/, or
  * EXCLUDED: listed below with the reason a binding does not need it.

A new helper in neither list, or a stale entry, fails the check, so the
decision is made when the helper is added rather than when a binding
author trips over a missing symbol.

Usage: python3 scripts/ci/check_inline_exports.py [repo_root]
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

# Helpers a binding needs, mapped to their exported wrapper.
EXPORTED = {
    "cwist_error_is_ok": "cwist_error_is_ok_extern",
    "cwist_endpoint_has": "cwist_endpoint_has_extern",
}

_CRYPTO = ("SHA-256/HMAC primitives used inside the library; a binding uses its own "
           "crypto crate, and exporting them would add a second public crypto API")
_PROTOBUF = ("protobuf wire-format load helpers used by the gRPC codec internals; not "
             "part of the v3.8 binding scope (app, routing, request/response)")
_SCOPED = ("cleanup-attribute plumbing for CWIST_DEFER_FREE / cwist_scratch_t scoped C "
           "locals; meaningless outside C")
_WASM = ("WASM guest entry glue compiled into the module that includes the header; "
         "hosts call the module's exported entry points, not a native symbol")

# Helpers deliberately left inline-only, with why.
EXCLUDED = {
    "cwist_sha256_rotr": _CRYPTO,
    "cwist_sha256_transform": _CRYPTO,
    "cwist_sha256_init": _CRYPTO,
    "cwist_sha256_update": _CRYPTO,
    "cwist_sha256_final": _CRYPTO,
    "cwist_hmac_sha256": _CRYPTO,
    "cwist_pb_load32_le": _PROTOBUF,
    "cwist_pb_load64_le": _PROTOBUF,
    "cwist_pb_float_bits": _PROTOBUF,
    "cwist_pb_double_bits": _PROTOBUF,
    "cwist_pb_load_float": _PROTOBUF,
    "cwist_pb_load_double": _PROTOBUF,
    "cwist_defer_free_cb": _SCOPED,
    "cwist_scratch_alloc": _SCOPED,
    "cwist_scratch_cleanup": _SCOPED,
    "cwist_wasm_dispatch_memory": _WASM,
    "cwist_wasm_free": _WASM,
    "cwist_wasm_component_dispatch": _WASM,
    "cwist_wasm_component_dispose": _WASM,
    "cwist_wasm_component_dispatch_stream": _WASM,
}

INLINE_RE = re.compile(r"^\s*static\s+inline\s+[^;{()]*?\b([A-Za-z_]\w*)\s*\(", re.M)


def public_headers(root: Path) -> list[Path]:
    base = root / "include" / "cwist"
    return sorted(p for p in base.rglob("*.h") if "vendor" not in p.relative_to(base).parts)


def inline_helpers(root: Path) -> dict[str, list[str]]:
    """Map each static inline helper name to the headers defining it."""
    found: dict[str, list[str]] = {}
    for header in public_headers(root):
        text = header.read_text(encoding="utf-8", errors="replace")
        for name in INLINE_RE.findall(text):
            found.setdefault(name, []).append(header.relative_to(root).as_posix())
    return found


def declared(root: Path, symbol: str) -> bool:
    """A non-static prototype `... symbol(...);` in some public header."""
    proto = re.compile(r"^(?!\s*static\b)[^\n;{}#]*\b" + re.escape(symbol) + r"\s*\([^;{]*\);",
                       re.M)
    return any(proto.search(h.read_text(encoding="utf-8", errors="replace"))
               for h in public_headers(root))


def defined(root: Path, symbol: str) -> bool:
    """A non-static definition `... symbol(...) {` somewhere under src/."""
    body = re.compile(r"^(?!\s*static\b)[^\n;{}#]*\b" + re.escape(symbol) + r"\s*\([^;{]*\)\s*\{",
                      re.M)
    return any(body.search(src.read_text(encoding="utf-8", errors="replace"))
               for src in (root / "src").rglob("*.c"))


def check(root: Path) -> list[str]:
    problems = []
    helpers = inline_helpers(root)
    for name, headers in sorted(helpers.items()):
        if name in EXPORTED and name in EXCLUDED:
            problems.append(f"{name}: listed as both EXPORTED and EXCLUDED")
        elif name not in EXPORTED and name not in EXCLUDED:
            problems.append(
                f"{name} ({', '.join(headers)}): new static inline helper in a public header; "
                "export a <name>_extern() wrapper or add it to EXCLUDED with a reason")
    for name, wrapper in sorted(EXPORTED.items()):
        if name not in helpers:
            problems.append(f"EXPORTED entry {name} no longer matches a static inline helper")
            continue
        if not declared(root, wrapper):
            problems.append(f"{wrapper}() (for {name}) is not declared in a public header")
        if not defined(root, wrapper):
            problems.append(f"{wrapper}() (for {name}) has no non-static definition in src/")
    for name in sorted(EXCLUDED):
        if name not in helpers:
            problems.append(f"EXCLUDED entry {name} no longer matches a static inline helper")
    return problems


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parents[2]
    problems = check(root)
    for p in problems:
        print(f"FAIL: {p}")
    if problems:
        return 1
    print(f"OK: every static inline helper in include/cwist is exported "
          f"({len(EXPORTED)}) or excluded with a reason ({len(EXCLUDED)}).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
