#!/bin/bash
# Build a NetBSD/aarch64 kernel with ncc and (optionally) boot-test it.
#
# Entry point for the netbsd test corpus. Same shape as
# tests/sqlite/build.sh, tests/cpython/build.sh.
#
# Defaults that "just work" if the user has the standard layout:
#   - this script is at <ncc-repo>/tests/netbsd/build.sh
#   - NetBSD source/build tree at ~/netbsd ({src,obj,tooldir})
#   - Docker available; image netbsd-build will be built on first use
#
# Usage:
#   ./build.sh                 build MINIMAL_VIRT64 (the booting config)
#   ./build.sh GENERIC64       build the full GENERIC64 kernel
#   ./build.sh MINIMAL_VIRT64 boot   build, then boot-test the result
#
# Env overrides:
#   NETBSD_DIR=path        (default ~/netbsd)
#   DOCKER_IMAGE=tag       (default netbsd-build)
#   KERNEL=name            (default MINIMAL_VIRT64; positional arg wins)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

KERNEL="${1:-${KERNEL:-MINIMAL_VIRT64}}"
DO_BOOT="${2:-}"
NETBSD_DIR="${NETBSD_DIR:-$HOME/netbsd}"
DOCKER_IMAGE="${DOCKER_IMAGE:-netbsd-build}"

if [ ! -d "$NETBSD_DIR/src" ] || [ ! -d "$NETBSD_DIR/obj" ] || [ ! -d "$NETBSD_DIR/tooldir" ]; then
  echo "ERROR: \$NETBSD_DIR=$NETBSD_DIR must contain src/, obj/, tooldir/." >&2
  echo "       Set NETBSD_DIR to your NetBSD source/build tree." >&2
  exit 1
fi

# Sync our kernel config into the NetBSD source tree's conf/ dir so
# config(8) and the build can find it. Idempotent; only the configs
# we wrote (MINIMAL_VIRT64) live in this repo.
CONFIG_SRC="$SCRIPT_DIR/$KERNEL"
CONFIG_DST="$NETBSD_DIR/src/sys/arch/evbarm/conf/$KERNEL"
if [ -f "$CONFIG_SRC" ]; then
  if ! cmp -s "$CONFIG_SRC" "$CONFIG_DST" 2>/dev/null; then
    echo "Syncing kernel config: $CONFIG_SRC -> $CONFIG_DST"
    cp "$CONFIG_SRC" "$CONFIG_DST"
  fi
fi

# Ensure Docker image exists.
if ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
  echo "=== Building Docker image $DOCKER_IMAGE ==="
  docker build -t "$DOCKER_IMAGE" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR"
fi

# Build the kernel.
XV6_DIR="$REPO_DIR" \
NETBSD_DIR="$NETBSD_DIR" \
DOCKER_IMAGE="$DOCKER_IMAGE" \
KERNEL="$KERNEL" \
  bash "$SCRIPT_DIR/tools/docker-kernel-build.sh"

KERNEL_IMG="$NETBSD_DIR/obj/sys/arch/evbarm/compile/$KERNEL/netbsd.img"
echo
echo "=== Built: $KERNEL_IMG ==="
ls -l "$KERNEL_IMG"

# Optional boot test.
if [ "$DO_BOOT" = "boot" ]; then
  echo
  bash "$SCRIPT_DIR/tools/boot-test.sh" "$KERNEL_IMG"
fi
