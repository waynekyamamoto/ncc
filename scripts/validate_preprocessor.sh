#!/bin/bash
# validate_preprocessor.sh — diff two preprocessor implementations on the
# Phase 2 validation corpus. Analog of validate_tokenizer.sh.
#
# Usage:
#   validate_preprocessor.sh                # default: ncc2 vs ncc2 (sanity)
#   validate_preprocessor.sh A              # diff A's -E against ncc2's -E
#   validate_preprocessor.sh A B            # diff A's -E against B's -E
#
# Compares the output of `<ncc> -E <file>` between A and B. When A and B
# are both ncc binaries, output should be byte-identical (any diff is a
# regression in the implementation under test).
#
# For Phase 2 development: build the spec-derived preprocessor as a
# separate binary (e.g., ncc-v2 via Makefile dual-build, mirroring the
# Phase 1 tokenizer pattern), then run:
#
#   validate_preprocessor.sh ncc-v2 ncc2
#
# A FAIL means the new preprocessor produces different preprocessed
# output than the canonical reference (current ncc2) on at least one
# corpus file. Diff is shown to stderr.
#
# Exit: 0 = all corpus files match; 1 = at least one mismatch.

set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

NCC_A="${1:-$ROOT/ncc2}"
NCC_B="${2:-$ROOT/ncc2}"

# Resolve to absolute paths so bash invocations don't fall back to PATH lookup
# when given relative names without a leading ./
resolve() {
    case "$1" in
        /*) echo "$1" ;;
        *)  echo "$ROOT/$1" ;;
    esac
}
NCC_A=$(resolve "$NCC_A")
NCC_B=$(resolve "$NCC_B")

if [ ! -x "$NCC_A" ]; then
    echo "no $NCC_A" >&2; exit 1
fi
if [ ! -x "$NCC_B" ]; then
    echo "no $NCC_B" >&2; exit 1
fi

# Validation corpus.  Mirrors validate_tokenizer.sh, but the preprocessor
# is more sensitive to #include resolution and macro expansion than the
# tokenizer is, so the same corpus exercises the preprocessor harder.
CORPUS=(
    # ncc's own source — heavy include + macro use via cc.h
    "$ROOT/src/tokenize.c"
    "$ROOT/src/parse.c"
    "$ROOT/src/preprocess.c"
    "$ROOT/src/codegen_arm64.c"
    "$ROOT/src/type.c"
    "$ROOT/src/main.c"
    "$ROOT/src/alloc.c"
    "$ROOT/src/hashmap.c"
    "$ROOT/src/unicode.c"
    # Real-world: sqlite3.c is ~256k lines and the preprocessor's
    # most heavily-exercised input in the project
    "$ROOT/tests/sqlite/sqlite3.c"
    "$ROOT/tests/sqlite/test_sqlite.c"
)
# Pick up regression repros (each one targets a specific pattern)
for f in "$ROOT"/tests/regression/[0-9][0-9]_*.c; do
    [ -f "$f" ] && CORPUS+=("$f")
done

# Optional: include cpython ceval.c if extracted (largest single C file
# in the project's cross-validation corpus)
if [ -f /tmp/Python-3.12.3/Python/ceval.c ]; then
    CORPUS+=("/tmp/Python-3.12.3/Python/ceval.c")
fi

WORKDIR=$(mktemp -d)
trap "rm -rf $WORKDIR" EXIT

PASS=0
FAIL=0
SKIP=0
FAIL_LIST=""

for src in "${CORPUS[@]}"; do
    if [ ! -f "$src" ]; then
        SKIP=$((SKIP+1))
        continue
    fi
    name="${src#$ROOT/}"
    A_OUT="$WORKDIR/$(echo "$name" | tr / _).A"
    B_OUT="$WORKDIR/$(echo "$name" | tr / _).B"

    # cpython has its own headers; pass through compile flags it expects
    case "$name" in
        */Python-3.12.3/*)
            EXTRA_FLAGS="-DPy_BUILD_CORE -DNDEBUG -I/tmp/Python-3.12.3/Include -I/tmp/Python-3.12.3/Include/internal -I/tmp/Python-3.12.3" ;;
        tests/sqlite/*)
            EXTRA_FLAGS="-DSQLITE_MEMORY_BARRIER=" ;;
        *)
            EXTRA_FLAGS="" ;;
    esac

    "$NCC_A" -E $EXTRA_FLAGS "$src" > "$A_OUT" 2>"$WORKDIR/A.err" || {
        echo "  A errored on $name; first stderr line:" >&2
        head -1 "$WORKDIR/A.err" >&2
        FAIL=$((FAIL+1))
        FAIL_LIST="$FAIL_LIST $name"
        continue
    }
    "$NCC_B" -E $EXTRA_FLAGS "$src" > "$B_OUT" 2>"$WORKDIR/B.err" || {
        echo "  B errored on $name; first stderr line:" >&2
        head -1 "$WORKDIR/B.err" >&2
        FAIL=$((FAIL+1))
        FAIL_LIST="$FAIL_LIST $name"
        continue
    }
    if cmp -s "$A_OUT" "$B_OUT"; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
        FAIL_LIST="$FAIL_LIST $name"
        echo "  diff $name (first 10 lines):" >&2
        diff "$A_OUT" "$B_OUT" | head -10 >&2
    fi
done

echo "preprocessor corpus: PASS=$PASS FAIL=$FAIL SKIP=$SKIP (of ${#CORPUS[@]})"
[ "$FAIL" -eq 0 ] || { echo "fail list:$FAIL_LIST" >&2; exit 1; }
