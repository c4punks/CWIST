#!/usr/bin/env python3
"""Size gate for the bare-metal smoke firmware (issue #447 Phase 5).

For each ELF built by `make baremetal-smoke`, measures

  flash       bytes the image occupies in flash: code, read-only data, the
              exception tables, init/fini arrays and the .data load image
  static_ram  .data + .bss (the linker's .heap region is not counted)
  heap        heap high-water mark the firmware reported (<name>.log)
  stack       stack high-water mark the firmware reported (<name>.log)
  allocs      allocation calls per steady-state request (<name>.log)

and fails when any value exceeds its limit in the budget file. The budget
file holds one `<firmware> <metric> <limit>` line per limit; `#` starts
a comment. A firmware or metric without a limit is reported, not gated.

Usage: baremetal_size_gate.py [--size arm-none-eabi-size] --budget FILE ELF...
Exit 0 = within budget, 1 = over budget or a measurement is missing.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

FLASH_SECTIONS = {".isr_vector", ".text", ".rodata", ".ARM.exidx", ".ARM.extab",
                  ".preinit_array", ".init_array", ".fini_array", ".data"}
RAM_SECTIONS = {".data", ".bss"}
METRICS = ("flash", "static_ram", "heap", "stack", "allocs")
HIGH_WATER = re.compile(r"heap high water: (\d+) bytes, stack used: (\d+) bytes")
ALLOCS = re.compile(r"steady allocs per request: (\d+)")


def parse_size_a(text: str) -> dict[str, int]:
    """Section name -> size from `size -A` output."""
    sections: dict[str, int] = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith(".") and parts[1].isdigit():
            sections[parts[0]] = int(parts[1])
    return sections


def measure_sections(sections: dict[str, int]) -> dict[str, int]:
    return {
        "flash": sum(v for k, v in sections.items() if k in FLASH_SECTIONS),
        "static_ram": sum(v for k, v in sections.items() if k in RAM_SECTIONS),
    }


def parse_log(text: str) -> dict[str, int]:
    values = {}
    high_water = HIGH_WATER.search(text)
    if high_water:
        values.update(heap=int(high_water.group(1)), stack=int(high_water.group(2)))
    allocs = ALLOCS.search(text)
    if allocs:
        values["allocs"] = int(allocs.group(1))
    return values


def parse_budget(text: str) -> dict[tuple[str, str], int]:
    budget: dict[tuple[str, str], int] = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 3 or parts[1] not in METRICS or not parts[2].isdigit():
            raise ValueError(f"budget line {n}: expected '<firmware> <{'|'.join(METRICS)}> <limit>'")
        budget[(parts[0], parts[1])] = int(parts[2])
    return budget


def check(measured: dict[str, dict[str, int]], budget: dict[tuple[str, str], int]) -> list[str]:
    """Return one failure message per exceeded or unmeasurable budget entry."""
    failures = []
    for (fw, metric), limit in sorted(budget.items()):
        if fw not in measured:
            continue
        value = measured[fw].get(metric)
        if value is None:
            failures.append(f"{fw} {metric}: not measured (missing metric in {fw}.log?)")
        elif value > limit:
            unit = "allocations/request" if metric == "allocs" else "bytes"
            failures.append(f"{fw} {metric}: {value} {unit} > budget {limit} (+{value - limit})")
    return failures


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--size", default="arm-none-eabi-size")
    ap.add_argument("--budget", required=True, type=Path)
    ap.add_argument("elfs", nargs="+", type=Path)
    args = ap.parse_args(argv)

    budget = parse_budget(args.budget.read_text())
    measured: dict[str, dict[str, int]] = {}
    for elf in args.elfs:
        out = subprocess.run([args.size, "-A", str(elf)], check=True, capture_output=True,
                             text=True).stdout
        values = measure_sections(parse_size_a(out))
        log = elf.with_suffix(".log")
        if log.exists():
            values.update(parse_log(log.read_text(errors="replace")))
        measured[elf.stem] = values

    print(f"{'firmware':<12}" + "".join(f"{m:>22}" for m in METRICS))
    for fw, values in measured.items():
        cells = []
        for m in METRICS:
            v, lim = values.get(m), budget.get((fw, m))
            cell = "-" if v is None else str(v)
            if lim is not None:
                cell += f" / {lim}"
            cells.append(f"{cell:>22}")
        print(f"{fw:<12}" + "".join(cells))

    failures = check(measured, budget)
    for f in failures:
        print(f"FAIL {f}")
    if failures:
        print(f"baremetal_size_gate: {len(failures)} budget(s) exceeded. If the growth is intended, "
              "raise the limit in the budget file in the same PR and say why.")
        return 1
    print("baremetal_size_gate: all within budget")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
