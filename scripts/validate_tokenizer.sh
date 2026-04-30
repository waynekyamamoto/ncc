#!/bin/bash
# validate_tokenizer.sh — diff two tokenizer implementations on the
# Phase 1 validation corpus (per docs/specs/01_tokenizer.md §14).
#
# Usage:
#   validate_tokenizer.sh                # default: ncc2 vs ncc2 (sanity)
#   validate_tokenizer.sh A B            # diff A's -fdump-tokens vs B's
#   validate_tokenizer.sh A              # diff A's against last cached
#                                          dump in /tmp/ncc_tokens_ref
#
# Requires: each binary supports `-fdump-tokens` (see commit 07536b5).
#
# Returns 0 if the streams match for every file in the corpus, 1
# otherwise. Per-file diffs are printed to stderr; the summary goes
# to stdout.

set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

NCC_A="${1:-$ROOT/ncc2}"
NCC_B="${2:-$ROOT/ncc2}"

if [ ! -x "$NCC_A" ]; then
    echo "no $NCC_A" >&2; exit 1
fi
if [ ! -x "$NCC_B" ]; then
    echo "no $NCC_B" >&2; exit 1
fi

# Validation corpus per docs/specs/01_tokenizer.md §14.
# Tokenize plain C source; compare token streams.
CORPUS=(
    # ncc's own source
    "$ROOT/src/tokenize.c"
    "$ROOT/src/parse.c"
    "$ROOT/src/preprocess.c"
    "$ROOT/src/codegen_arm64.c"
    "$ROOT/src/type.c"
    "$ROOT/src/main.c"
    "$ROOT/src/alloc.c"
    "$ROOT/src/hashmap.c"
    "$ROOT/src/unicode.c"
    # Real-world inputs
    "$ROOT/tests/sqlite/sqlite3.c"
    "$ROOT/tests/sqlite/test_sqlite.c"
    # Regression repros (small, exercise specific patterns)
)
# Add all regression files
for f in "$ROOT"/tests/regression/[0-9][0-9]_*.c; do
    [ -f "$f" ] && CORPUS+=("$f")
done

# Optional: include cpython ceval.c if extracted
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
    "$NCC_A" -fdump-tokens "$src" > "$A_OUT" 2>"$WORKDIR/A.err" || {
        echo "  A errored on $name; first stderr line:" >&2
        head -1 "$WORKDIR/A.err" >&2
        FAIL=$((FAIL+1))
        FAIL_LIST="$FAIL_LIST $name"
        continue
    }
    "$NCC_B" -fdump-tokens "$src" > "$B_OUT" 2>"$WORKDIR/B.err" || {
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

echo "tokenizer corpus: PASS=$PASS FAIL=$FAIL SKIP=$SKIP (of ${#CORPUS[@]})"
[ "$FAIL" -eq 0 ] || { echo "fail list:$FAIL_LIST" >&2; exit 1; }
