#!/bin/bash
# ncc Linux kernel scan: compile every .c file in a subsystem and report PASS/FAIL.
#
# Usage:
#   scan.sh <label> <linux-subdir>      # scan one subsystem, print pass/fail summary
#   scan.sh --linux <path>              # override default Linux source path
#
# Environment overrides:
#   NCC          path to ncc binary       (default: ../../ncc relative to this script)
#   LINUX        path to Linux source     (default: $HOME/ncc-linux/linux)
#   FIX_HEADER   pre-include header       (default: ./linux_fix.h)
#   STUBS_DIR    stubs include dir        (default: ./stubs)
#   SKIP_LIST    one-file-per-line list   (default: ./skip_list.txt)

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NCC="${NCC:-$SCRIPT_DIR/../../ncc}"
LINUX="${LINUX:-$HOME/ncc-linux/linux}"
FIX_HEADER="${FIX_HEADER:-$SCRIPT_DIR/linux_fix.h}"
STUBS_DIR="${STUBS_DIR:-$SCRIPT_DIR/stubs}"
SKIP_LIST="${SKIP_LIST:-$SCRIPT_DIR/skip_list.txt}"

if [ "${1:-}" = "--linux" ]; then
  LINUX="$2"
  shift 2
fi

LABEL="${1:?usage: scan.sh <label> <linux-subdir>}"
DIR="${2:?usage: scan.sh <label> <linux-subdir>}"

PASS=0
FAIL=0
SKIP=0

is_skipped() {
  local rel="$1"
  [ -f "$SKIP_LIST" ] && grep -qxF "$rel" "$SKIP_LIST"
}

for f in "$DIR"/*.c; do
  [ -e "$f" ] || continue
  rel="${f#$LINUX/}"
  name=$(basename "$f" .c)
  if is_skipped "$rel"; then
    SKIP=$((SKIP+1))
    echo "SKIP: $rel"
    continue
  fi
  # Some subsystems include local headers from their own dir (e.g. mm/slub.c
  # does `#include "slab.h"`). Add the scanned dir as an include path.
  err=$(cd "$LINUX" && "$NCC" -S -o /dev/null \
    -include "$FIX_HEADER" \
    -I "$STUBS_DIR" \
    -I "${DIR#$LINUX/}" \
    -I include -I arch/arm64/include -I arch/arm64/include/generated \
    -I arch/arm64/include/generated/uapi -I arch/arm64/include/uapi \
    -I include/generated -I include/uapi \
    -D__KERNEL__ -D__LINUX_ARM_ARCH__=8 -D__ARM_ARCH=8 \
    -DKASAN_SHADOW_SCALE_SHIFT=3 -DCONFIG_NR_CPUS=8 \
    -DCONFIG_NODES_SHIFT=0 -DCONFIG_DYNAMIC_DEBUG -DCONFIG_REF_TRACKER \
    "$f" 2>&1)
  if [ -z "$err" ]; then
    PASS=$((PASS+1))
  else
    first=$(printf '%s' "$err" | head -1)
    echo "FAIL: $name - $first"
    FAIL=$((FAIL+1))
  fi
done
echo "=== $LABEL: PASS=$PASS FAIL=$FAIL SKIP=$SKIP ==="
