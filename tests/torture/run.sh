#!/bin/bash
# GCC torture test runner for ncc
# Usage: ./run.sh [test.c]  — run one test
#        ./run.sh           — run all tests
#        ./run.sh --summary — run all and show only summary

NCC="../../ncc"
PASS=0
FAIL_COMPILE=0
FAIL_RUNTIME=0
SKIP=0
TOTAL=0
COMPILE_ERRORS=""
RUNTIME_ERRORS=""
SUMMARY_ONLY=0

if [ "$1" = "--summary" ]; then
    SUMMARY_ONLY=1
    shift
fi

run_test() {
    local src="$1"
    local name=$(basename "$src" .c)
    TOTAL=$((TOTAL+1))

    # Compile with ncc
    if ! $NCC -o "/tmp/torture_ncc_${name}" "$src" -lm 2>/dev/null; then
        FAIL_COMPILE=$((FAIL_COMPILE+1))
        COMPILE_ERRORS="$COMPILE_ERRORS $name"
        [ $SUMMARY_ONLY -eq 0 ] && echo "FAIL(compile): $name"
        return
    fi

    # Run with timeout
    if ! perl -e 'alarm 5; exec @ARGV' -- "/tmp/torture_ncc_${name}" >/dev/null 2>&1; then
        FAIL_RUNTIME=$((FAIL_RUNTIME+1))
        RUNTIME_ERRORS="$RUNTIME_ERRORS $name"
        [ $SUMMARY_ONLY -eq 0 ] && echo "FAIL(runtime): $name"
        return
    fi

    PASS=$((PASS+1))
    [ $SUMMARY_ONLY -eq 0 ] && echo "PASS: $name"
}

if [ -n "$1" ]; then
    run_test "$1"
else
    for f in *.c; do
        run_test "$f"
    done
fi

echo ""
echo "=== TOTAL=$TOTAL PASS=$PASS FAIL_COMPILE=$FAIL_COMPILE FAIL_RUNTIME=$FAIL_RUNTIME ==="
echo "Pass rate: $(( PASS * 100 / TOTAL ))%"

if [ -n "$COMPILE_ERRORS" ]; then
    echo ""
    echo "Compile failures:$COMPILE_ERRORS" | fold -s -w 80
fi
if [ -n "$RUNTIME_ERRORS" ]; then
    echo ""
    echo "Runtime failures:$RUNTIME_ERRORS" | fold -s -w 80
fi
