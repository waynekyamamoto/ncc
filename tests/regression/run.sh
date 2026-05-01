#!/bin/bash
# Regression suite: minimal repros for compiler bugs that have been fixed.
# Each .c file must compile cleanly with ncc (no errors, no crash).
# Some files exercise _Static_assert, which is enough verification.
#
# Usage: ./run.sh [test.c]   — single test
#        ./run.sh            — all tests

set -u
NCC="${NCC:-../../ncc2}"
PASS=0
FAIL=0
FAIL_NAMES=""

run_test() {
  local src="$1"
  local name
  name=$(basename "$src" .c)
  local out
  if out=$("$NCC" -S -o /dev/null "$src" 2>&1) && [ -z "$out" ]; then
    PASS=$((PASS+1))
    echo "PASS: $name"
  else
    FAIL=$((FAIL+1))
    FAIL_NAMES="$FAIL_NAMES $name"
    echo "FAIL: $name"
    [ -n "$out" ] && echo "  $out" | head -3
  fi
}

if [ $# -eq 1 ]; then
  run_test "$1"
else
  for f in [0-9][0-9]_*.c; do
    [ -e "$f" ] || continue
    run_test "$f"
  done
fi

echo
echo "=== regression: PASS=$PASS FAIL=$FAIL ==="
[ "$FAIL" -eq 0 ]
