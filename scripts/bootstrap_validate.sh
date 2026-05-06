#!/bin/bash
# bootstrap_validate.sh — confirm ncc reaches a fixed point compiling itself.
#
#   stage1 = host_ncc compiling src/*.c
#   stage2 = stage1   compiling src/*.c
#   pass <=> md5(stage1/ncc) == md5(stage2/ncc)
#
# Usage:
#   bootstrap_validate.sh                 # bootstrap canonical ./ncc
#
# Run from repo root. Exits 0 on fixed point, 1 on mismatch, 2 on build failure.

set -e

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

NCC="${NCC:-$ROOT/ncc}"
BINNAME="ncc"

if [ ! -x "$NCC" ]; then
    echo "no $NCC binary; run 'make' first" >&2
    exit 2
fi

STAGE1="stage1"
STAGE2="stage2"

mkdir -p "$STAGE1" "$STAGE2"
[ -e "$STAGE1/include" ] || ln -sf "$ROOT/include" "$STAGE1/include"
[ -e "$STAGE2/include" ] || ln -sf "$ROOT/include" "$STAGE2/include"

rm -f "$STAGE1"/*.o "$STAGE1/$BINNAME" "$STAGE2"/*.o "$STAGE2/$BINNAME"

echo "stage1: building $BINNAME with host $NCC"
for f in $(ls src/*.c); do
    "$NCC" -c -o "$STAGE1/$(basename "${f%.c}").o" "$f" \
        || { echo "stage1 compile failed on $f" >&2; exit 2; }
done
"$NCC" -o "$STAGE1/$BINNAME" "$STAGE1"/*.o \
    || { echo "stage1 link failed" >&2; exit 2; }

echo "stage2: building $BINNAME with $STAGE1/$BINNAME"
for f in $(ls src/*.c); do
    "$STAGE1/$BINNAME" -c -o "$STAGE2/$(basename "${f%.c}").o" "$f" \
        || { echo "stage2 compile failed on $f" >&2; exit 2; }
done
"$STAGE1/$BINNAME" -o "$STAGE2/$BINNAME" "$STAGE2"/*.o \
    || { echo "stage2 link failed" >&2; exit 2; }

ln -sf "stage2/ncc" ncc2

S1=$(md5 -q "$STAGE1/$BINNAME")
S2=$(md5 -q "$STAGE2/$BINNAME")

echo "stage1 md5: $S1"
echo "stage2 md5: $S2"

if [ "$S1" = "$S2" ]; then
    echo "FIXED POINT: stage1 == stage2 ($BINNAME)"
    exit 0
else
    echo "MISMATCH: stage1 != stage2 ($BINNAME)" >&2
    exit 1
fi
