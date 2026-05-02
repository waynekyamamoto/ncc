#!/bin/bash
# bootstrap_validate.sh — confirm ncc reaches a fixed point compiling itself.
#
#   stage1 = host_ncc compiling src/*.c
#   stage2 = stage1   compiling src/*.c
#   pass <=> md5(stage1/ncc) == md5(stage2/ncc)
#
# Run from repo root. Exits 0 on fixed point, 1 on mismatch, 2 on build failure.

set -e

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

if [ ! -x "$ROOT/ncc" ]; then
    echo "no ./ncc binary; run 'make ncc' first" >&2
    exit 2
fi

mkdir -p stage1 stage2
[ -e stage1/include ] || ln -sf "$ROOT/include" stage1/include
[ -e stage2/include ] || ln -sf "$ROOT/include" stage2/include

rm -f stage1/*.o stage1/ncc stage2/*.o stage2/ncc

echo "stage1: building ncc with host ncc"
for f in src/*.c; do
    ./ncc -c -o "stage1/$(basename "${f%.c}").o" "$f" || { echo "stage1 compile failed on $f" >&2; exit 2; }
done
./ncc -o stage1/ncc stage1/*.o || { echo "stage1 link failed" >&2; exit 2; }

echo "stage2: building ncc with stage1/ncc"
for f in src/*.c; do
    stage1/ncc -c -o "stage2/$(basename "${f%.c}").o" "$f" || { echo "stage2 compile failed on $f" >&2; exit 2; }
done
stage1/ncc -o stage2/ncc stage2/*.o || { echo "stage2 link failed" >&2; exit 2; }

ln -sf stage2/ncc ncc2

# Byte-compare with cmp (POSIX, portable across macOS + Linux Docker).
# Print a hash for the log if either md5 or md5sum is available.
if command -v md5 >/dev/null 2>&1; then
    echo "stage1 md5: $(md5 -q stage1/ncc)"
    echo "stage2 md5: $(md5 -q stage2/ncc)"
elif command -v md5sum >/dev/null 2>&1; then
    echo "stage1 md5: $(md5sum stage1/ncc | awk '{print $1}')"
    echo "stage2 md5: $(md5sum stage2/ncc | awk '{print $1}')"
fi

if cmp -s stage1/ncc stage2/ncc; then
    echo "FIXED POINT: stage1 == ncc2"
    exit 0
else
    echo "MISMATCH: stage1 != ncc2" >&2
    exit 1
fi
