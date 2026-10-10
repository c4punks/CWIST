#!/usr/bin/env bash
# Static analysis gate: cppcheck over src/ with the warning, performance and
# portability checks. The tree is clean at these settings, so any finding
# fails the gate. Fix it, or, if it is a false positive, suppress that one
# site with an inline comment that says why:
#     // cppcheck-suppress <id> -- <reason>
#
# Pinned to the cppcheck that ubuntu-24.04 ships (2.13); other versions add
# or drop checks, so a different local version may disagree with CI.
set -u

CPPCHECK="${CPPCHECK:-cppcheck}"
command -v "$CPPCHECK" >/dev/null || { echo "cppcheck.sh: $CPPCHECK not found" >&2; exit 2; }
echo "using $("$CPPCHECK" --version)"

out=$(mktemp)
trap 'rm -f "$out"' EXIT
"$CPPCHECK" \
    --enable=warning,performance,portability \
    --std=c11 \
    -j "$(nproc)" \
    -I include \
    -D__linux__ -D_GNU_SOURCE \
    --inline-suppr \
    --suppress=missingIncludeSystem \
    --suppress=missingInclude \
    --suppress=unmatchedSuppression \
    --template='{file}:{line}: {severity}: {message} [{id}]' \
    --quiet \
    src 2>"$out"
rc=$?
if [ "$rc" -ne 0 ]; then
    cat "$out" >&2
    echo "cppcheck.sh: cppcheck exited $rc" >&2
    exit 2
fi
if [ -s "$out" ]; then
    cat "$out"
    echo "cppcheck.sh: $(wc -l <"$out") finding(s); FAIL"
    exit 1
fi
echo "cppcheck.sh: no findings; PASS"
