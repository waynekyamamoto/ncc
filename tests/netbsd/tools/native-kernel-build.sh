#!/bin/bash
# Build a NetBSD/evbarm64 kernel natively on Linux with CC=ncc2.
# Replaces docker-kernel-build.sh for native Ubuntu builds.
#
# Required env vars (or defaults):
#   NCC_REPO    - path to ncc repo (default: this script's ../../..)
#   NETBSD_DIR  - path to NetBSD source/build tree (default: ~/netbsd)
#   KERNEL      - kernel config name (default: MINIMAL_VIRT64)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NCC_REPO="${NCC_REPO:-$(cd "$SCRIPT_DIR/../../.." && pwd)}"
NETBSD_DIR="${NETBSD_DIR:-$HOME/netbsd}"
KERNEL="${KERNEL:-MINIMAL_VIRT64}"

NCC2="$NCC_REPO/ncc2"
WRAPPER="$SCRIPT_DIR/ncc-elf-wrapper.sh"
NBMAKE="$NETBSD_DIR/tooldir/bin/nbmake-evbarm"
NBCONFIG="$NETBSD_DIR/tooldir/bin/nbconfig"
BUILD_DIR="$NETBSD_DIR/obj/sys/arch/evbarm/compile/$KERNEL"

if [ ! -x "$NCC2" ]; then
  echo "ERROR: $NCC2 not found — run 'make' then 'scripts/bootstrap_validate.sh' first" >&2
  exit 1
fi
if [ ! -x "$NBMAKE" ]; then
  echo "ERROR: $NBMAKE not found — run build.sh tools first" >&2
  exit 1
fi

echo "=== Syncing kernel config ==="
CONFIG_SRC="$SCRIPT_DIR/../$KERNEL"
CONFIG_DST="$NETBSD_DIR/src/sys/arch/evbarm/conf/$KERNEL"
if [ -f "$CONFIG_SRC" ]; then
  if ! cmp -s "$CONFIG_SRC" "$CONFIG_DST" 2>/dev/null; then
    echo "  $CONFIG_SRC -> $CONFIG_DST"
    cp "$CONFIG_SRC" "$CONFIG_DST"
  fi
fi

echo "=== Regenerating build dir from $KERNEL config ==="
mkdir -p "$BUILD_DIR"
cd "$NETBSD_DIR/src/sys/arch/evbarm/conf"
"$NBCONFIG" -b "$BUILD_DIR" -s "$NETBSD_DIR/src/sys" "$KERNEL"

echo "=== Re-pointing build-dir symlinks ==="
cd "$BUILD_DIR"
ln -sfn "$NETBSD_DIR/src/sys/arch/evbarm/include" machine
ln -sfn "$NETBSD_DIR/src/sys/arch/arm/include" arm
ln -sfn "$NETBSD_DIR/src/sys/arch/aarch64/include" aarch64
ln -sfn "$NETBSD_DIR/src/sys/arch/evbarm/include" evbarm

echo "=== Forcing full rebuild: removing .o + linker outputs ==="
find . -name '*.o' -delete
rm -f netbsd netbsd.img netbsd.bin netbsd.gdb netbsd.map "netbsd-$KERNEL.debug"

echo "=== Running kernel build with CC=ncc2 ==="
exec "$NBMAKE" -j 1 \
  TOOL_CC.gcc="$WRAPPER" \
  CC="$WRAPPER" \
  all
