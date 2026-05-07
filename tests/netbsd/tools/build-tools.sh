#!/bin/bash
# build-tools.sh — Bootstrap the NetBSD/aarch64 cross-toolchain.
#
# This is the one-time, slow step (~30 min). It runs NetBSD's own build.sh
# in `tools` mode, which uses the HOST gcc to compile a cross-toolchain
# (aarch64--netbsd-gcc, aarch64--netbsd-as, aarch64--netbsd-ld) plus
# NetBSD's host-side build tools (nbmake-evbarm, nbconfig, nbmakefs, dbsym).
# These artifacts land in $NETBSD_DIR/tooldir/ and $NETBSD_DIR/obj/tools/.
#
# IMPORTANT: this step does NOT use ncc. The cross-toolchain that ncc
# eventually relies on for assembly + linking is itself gcc-built. ncc
# only takes over for kernel C compilation in `make netbsd`.
#
# Platform behavior:
#   Linux  — runs build.sh natively with the host gcc.
#   Darwin — runs build.sh inside a Linux Docker container, so the
#            tooldir binaries are Linux ELF and match the kernel-build
#            container in tools/docker-kernel-build.sh.
#
# Env overrides:
#   NETBSD_DIR    (default ~/netbsd)
#   DOCKER_IMAGE  (default netbsd-build; macOS only)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NETBSD_DIR="${NETBSD_DIR:-$HOME/netbsd}"
DOCKER_IMAGE="${DOCKER_IMAGE:-netbsd-build}"
NBMAKE="$NETBSD_DIR/tooldir/bin/nbmake-evbarm"

if [ ! -d "$NETBSD_DIR/src" ] || [ ! -x "$NETBSD_DIR/src/build.sh" ]; then
  cat >&2 <<EOF
ERROR: NetBSD source not found at $NETBSD_DIR/src.

Clone netbsd-10 first:

  mkdir -p $NETBSD_DIR
  git clone --branch netbsd-10 https://github.com/NetBSD/src.git $NETBSD_DIR/src

Or set NETBSD_DIR to point at an existing checkout:

  NETBSD_DIR=/path/to/netbsd make netbsd-tools

EOF
  exit 1
fi

# Idempotency: if tooldir is already present, do nothing.
if [ -x "$NBMAKE" ]; then
  echo "Cross-toolchain already built at $NETBSD_DIR/tooldir."
  echo "Delete $NETBSD_DIR/tooldir + $NETBSD_DIR/obj/tools to force a rebuild."
  exit 0
fi

UNAME="$(uname -s)"

cat <<EOF
=== Building NetBSD cross-toolchain ===
Source:  $NETBSD_DIR/src
Output:  $NETBSD_DIR/tooldir   (binaries)
         $NETBSD_DIR/obj/tools (build trees)

This is the gcc-built cross-toolchain prerequisite step.
It runs NetBSD's build.sh -U tools and produces aarch64--netbsd-gcc,
nbmake-evbarm, nbconfig, nbmakefs, dbsym, etc.

Expected duration: ~30 minutes on a recent box. One-time per machine.
ncc is NOT used in this step — the kernel build (make netbsd) is.

EOF

case "$UNAME" in
  Linux)
    echo "Host: Linux — running build.sh natively with host gcc."
    echo
    cd "$NETBSD_DIR/src"
    exec ./build.sh -U -O "$NETBSD_DIR/obj" -T "$NETBSD_DIR/tooldir" tools
    ;;
  Darwin)
    if ! command -v docker >/dev/null 2>&1; then
      cat >&2 <<EOF
ERROR: Docker not found. On macOS, this step runs inside a Linux container
so the resulting tooldir binaries are Linux ELF (matching the kernel-build
container).

Install Docker Desktop from https://www.docker.com/products/docker-desktop
then re-run: make netbsd-tools

EOF
      exit 1
    fi

    if ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
      echo "=== Building Docker image $DOCKER_IMAGE ==="
      docker build -t "$DOCKER_IMAGE" -f "$SCRIPT_DIR/../Dockerfile" "$SCRIPT_DIR/.."
    fi

    echo "Host: macOS — running build.sh inside $DOCKER_IMAGE container."
    echo
    exec docker run --rm -i \
      -v "$NETBSD_DIR":/netbsd \
      "$DOCKER_IMAGE" \
      bash -c 'cd /netbsd/src && ./build.sh -U -O /netbsd/obj -T /netbsd/tooldir tools'
    ;;
  *)
    echo "ERROR: unsupported host OS: $UNAME" >&2
    echo "Supported: Linux (native), Darwin (Docker)." >&2
    exit 1
    ;;
esac
