#!/bin/bash
# Compliance test runner: compile with both clang and ncc, compare output
# Usage: ./run.sh [test.c] or ./run.sh (runs all)

NCC="${NCC:-../../ncc2}"
# Use clang if available, fall back to gcc
if command -v clang >/dev/null 2>&1; then
    REF_CC=clang
elif command -v gcc >/dev/null 2>&1; then
    REF_CC=gcc
else
    echo "no reference compiler (clang or gcc) found" >&2
    exit 1
fi
PASS=0
FAIL=0
ERRORS=""

run_test() {
  local src="$1"
  local name=$(basename "$src" .c)

  # Compile with reference compiler
  if ! $REF_CC -o "/tmp/compliance_clang_${name}" "$src" 2>/dev/null; then
    echo "SKIP: $name ($REF_CC compile failed)"
    return
  fi

  # Compile with ncc
  if ! $NCC -o "/tmp/compliance_ncc_${name}" "$src" 2>/dev/null; then
    echo "FAIL: $name (ncc compile failed)"
    FAIL=$((FAIL+1))
    ERRORS="$ERRORS\n  $name: compile error"
    return
  fi

  # Run both
  expected=$("/tmp/compliance_clang_${name}" 2>&1)
  actual=$("/tmp/compliance_ncc_${name}" 2>&1)

  if [ "$expected" = "$actual" ]; then
    echo "PASS: $name"
    PASS=$((PASS+1))
  else
    echo "FAIL: $name"
    echo "  clang: $expected"
    echo "  ncc:   $actual"
    FAIL=$((FAIL+1))
    ERRORS="$ERRORS\n  $name: output mismatch"
  fi
}

if [ -n "$1" ]; then
  run_test "$1"
else
  for f in *.c; do
    [ -f "$f" ] && run_test "$f"
  done
fi

echo ""
echo "=== PASS=$PASS FAIL=$FAIL ==="
if [ -n "$ERRORS" ]; then
  echo -e "Failures:$ERRORS"
fi
