#!/bin/bash
# Build a NetBSD/aarch64 kernel with ncc, on Linux or macOS, and (optionally)
# boot-test it.
#
# Platform behavior:
#   Linux  — runs tools/native-kernel-build.sh (no Docker needed).
#   Darwin — runs tools/docker-kernel-build.sh (kernel build inside a Linux
#            container; ncc and the cross-toolchain run there).
#
# Prerequisite: the cross-toolchain is already built (tools/build-tools.sh).
#
# Usage:
#   ./build.sh                       build MINIMAL_VIRT64
#   ./build.sh GENERIC64             build the full GENERIC64 kernel
#   ./build.sh MINIMAL_VIRT64 boot   build, then boot-test the result
#
# Env overrides:
#   NETBSD_DIR    (default ~/netbsd) — must contain src/, obj/, tooldir/
#   DOCKER_IMAGE  (default netbsd-build; macOS only)
#   KERNEL        (default MINIMAL_VIRT64; positional arg wins)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

KERNEL="${1:-${KERNEL:-MINIMAL_VIRT64}}"
DO_BOOT="${2:-}"
NETBSD_DIR="${NETBSD_DIR:-$HOME/netbsd}"
DOCKER_IMAGE="${DOCKER_IMAGE:-netbsd-build}"

# Prerequisite: NetBSD source + cross-toolchain.
if [ ! -d "$NETBSD_DIR/src" ]; then
  echo "ERROR: NetBSD source not found at $NETBSD_DIR/src." >&2
  echo "       Clone netbsd-10 then run: make netbsd-tools" >&2
  exit 1
fi
if [ ! -x "$NETBSD_DIR/tooldir/bin/nbmake-evbarm" ]; then
  echo "ERROR: cross-toolchain not built at $NETBSD_DIR/tooldir." >&2
  echo "       Run: make netbsd-tools" >&2
  exit 1
fi

# Sync our kernel config into the NetBSD source tree (idempotent).
CONFIG_SRC="$SCRIPT_DIR/$KERNEL"
CONFIG_DST="$NETBSD_DIR/src/sys/arch/evbarm/conf/$KERNEL"
if [ -f "$CONFIG_SRC" ]; then
  if ! cmp -s "$CONFIG_SRC" "$CONFIG_DST" 2>/dev/null; then
    echo "Syncing kernel config: $CONFIG_SRC -> $CONFIG_DST"
    cp "$CONFIG_SRC" "$CONFIG_DST"
  fi
fi

UNAME="$(uname -s)"
case "$UNAME" in
  Linux)
    if [ ! -x "$REPO_DIR/ncc2" ]; then
      echo "ERROR: ncc2 not found at $REPO_DIR/ncc2." >&2
      echo "       Run: make ncc && scripts/bootstrap_validate.sh" >&2
      exit 1
    fi
    echo "Host: Linux — native kernel build with ncc2."
    NETBSD_DIR="$NETBSD_DIR" KERNEL="$KERNEL" \
      bash "$SCRIPT_DIR/tools/native-kernel-build.sh"
    ;;
  Darwin)
    if ! command -v docker >/dev/null 2>&1; then
      echo "ERROR: Docker not found. macOS kernel build runs inside a Linux container." >&2
      echo "       Install Docker Desktop, then re-run." >&2
      exit 1
    fi
    if ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
      echo "=== Building Docker image $DOCKER_IMAGE ==="
      docker build -t "$DOCKER_IMAGE" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR"
    fi
    echo "Host: macOS — kernel build via $DOCKER_IMAGE container."
    XV6_DIR="$REPO_DIR" \
    NETBSD_DIR="$NETBSD_DIR" \
    DOCKER_IMAGE="$DOCKER_IMAGE" \
    KERNEL="$KERNEL" \
      bash "$SCRIPT_DIR/tools/docker-kernel-build.sh"
    ;;
  *)
    echo "ERROR: unsupported host OS: $UNAME" >&2
    exit 1
    ;;
esac

KERNEL_IMG="$NETBSD_DIR/obj/sys/arch/evbarm/compile/$KERNEL/netbsd.img"
echo
echo "=== Built: $KERNEL_IMG ==="
ls -l "$KERNEL_IMG"

if [ "$DO_BOOT" = "boot" ]; then
  echo
  bash "$SCRIPT_DIR/tools/boot-test.sh" "$KERNEL_IMG"
fi
