#!/bin/bash
# Compliance test runner for the AArch64 C compiler spec.
#
# Each test is self-checking: returns 0 on pass, non-zero on fail.
# This script compiles each test with $CC (default: cc), runs it, and
# reports pass/fail. A test that fails to compile counts as a failure.
#
# Usage:
#   ./run.sh                       # use $CC or 'cc'
#   CC=gcc ./run.sh
#   CC=clang CFLAGS="-O2" ./run.sh
#   CC=/path/to/your-compiler ./run.sh
#
# Exit status: 0 if all pass, 1 if any failed.

set -u

CC="${CC:-cc}"
CFLAGS="${CFLAGS:-}"
TMPDIR="${TMPDIR:-/tmp}"

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

PASS=0
FAIL=0
FAILED=()

printf '== AArch64 C compiler compliance suite ==\n'
printf 'CC=%s\n' "$CC"
[ -n "$CFLAGS" ] && printf 'CFLAGS=%s\n' "$CFLAGS"
printf '\n'

for src in [0-9][0-9]_*.c; do
    name="${src%.c}"
    bin="$TMPDIR/aarch64spec_${name}"
    cclog="$TMPDIR/aarch64spec_${name}.cc.log"
    runlog="$TMPDIR/aarch64spec_${name}.run.log"

    if ! $CC $CFLAGS -o "$bin" "$src" >"$cclog" 2>&1; then
        printf 'FAIL  %s  (compile; see %s)\n' "$name" "$cclog"
        FAIL=$((FAIL+1))
        FAILED+=("$name [compile]")
        continue
    fi

    if "$bin" >"$runlog" 2>&1; then
        printf 'PASS  %s\n' "$name"
        PASS=$((PASS+1))
        rm -f "$bin" "$cclog" "$runlog"
    else
        rc=$?
        printf 'FAIL  %s  (exit=%d; see %s)\n' "$name" "$rc" "$runlog"
        FAIL=$((FAIL+1))
        FAILED+=("$name [exit=$rc]")
    fi
done

printf '\nPass: %d   Fail: %d\n' "$PASS" "$FAIL"

if [ "$FAIL" -gt 0 ]; then
    printf '\nFailures:\n'
    for f in "${FAILED[@]}"; do
        printf '  - %s\n' "$f"
    done
    exit 1
fi
exit 0
