#!/usr/bin/env bash
# Regenerate docs/api/v4.0-api-baseline.txt, the recorded public API of
# include/cwist/** (names, function signatures, struct/union/enum layouts).
# Run via `make api-baseline`; `make api-check` is the CI gate that compares
# the headers against the committed file. See scripts/ci/api_baseline.py.
set -euo pipefail

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

OUT=docs/api/v4.0-api-baseline.txt
python3 scripts/ci/api_baseline.py . > "$OUT.tmp"
mv "$OUT.tmp" "$OUT"
echo "api-baseline: wrote $OUT"
