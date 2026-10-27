#!/usr/bin/env python3
import re
import sys
from pathlib import Path

ADMONITION_LABELS = {
    "NOTE": "Note",
    "TIP": "Tip",
    "IMPORTANT": "Important",
    "WARNING": "Warning",
    "CAUTION": "Caution",
}
ATX_HEADING = re.compile(r"^ {0,3}#{1,6}(?:[ \t]+|$)")
INLINE_CODE = re.compile(r"`([^`\r\n]+)`")
FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
HORIZONTAL_RULE = re.compile(r"^ {0,3}---[ \t]*(?:\r?\n)?$")
ADMONITION = re.compile(
    r"^(\s*>\s*)\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\][ \t]*(\r?\n)?$",
    re.IGNORECASE,
)


def clean_markdown(text: str) -> str:
    output = []
    fence_char = None
    previous_blank = True

    for line in text.splitlines(keepends=True):
        fence = FENCE.match(line)
        if fence:
            marker = fence.group(1)[0]
            if fence_char is None:
                fence_char = marker
            elif fence_char == marker:
                fence_char = None
            output.append(line)
            previous_blank = False
            continue

        if fence_char is None:
            if ATX_HEADING.match(line):
                line = INLINE_CODE.sub(r"\1", line)
            if previous_blank and HORIZONTAL_RULE.fullmatch(line):
                line = line.replace("---", "***", 1)
            admonition = ADMONITION.match(line)
            if admonition:
                label = ADMONITION_LABELS[admonition.group(2).upper()]
                newline = admonition.group(3) or ""
                line = f"{admonition.group(1)}**{label}:**{newline}"

        output.append(line)
        previous_blank = not line.strip()

    return "".join(output)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: doxygen_markdown_filter.py MARKDOWN_FILE", file=sys.stderr)
        return 2
    text = Path(sys.argv[1]).read_text(encoding="utf-8")
    sys.stdout.write(clean_markdown(text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
