#!/bin/bash
# Compliance test runner: compile with both clang and ncc, compare output
# Usage: ./run.sh [test.c] or ./run.sh (runs all)

NCC="../../ncc"
PASS=0
FAIL=0
ERRORS=""

run_test() {
  local src="$1"
  local name=$(basename "$src" .c)

  # Compile with clang
  if ! clang -o "/tmp/compliance_clang_${name}" "$src" 2>/dev/null; then
    echo "SKIP: $name (clang compile failed)"
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
