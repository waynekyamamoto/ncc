#!/bin/bash
# Build a NetBSD/evbarm64 kernel inside Docker with CC=ncc.
#
# KERNEL env var selects the config (default: GENERIC64). MINIMAL_VIRT64
# is the small diagnostic config that boots to root device: prompt.
#
# Required env vars:
#   XV6_DIR     - path to the ncc repo (https://github.com/waynekyamamoto/ncc).
#                 Bind-mounted to /xv6 inside the container; ncc-linux is built
#                 fresh from $XV6_DIR/src each run.  Wrapper scripts and stub
#                 sources live in tests/netbsd/{tools,stubs}/ inside this repo.
#   NETBSD_DIR  - path to the NetBSD source/build tree containing src/, obj/,
#                 and tooldir/.  Bind-mounted to /netbsd.
#
# Optional:
#   DOCKER_IMAGE - tag of the Docker build image (default: netbsd-build).
#                  Build with `docker build -t netbsd-build -f tests/netbsd/Dockerfile .`
#                  from the ncc repo root before running this script.

set -e

: "${XV6_DIR:?XV6_DIR is required (path to ncc repo)}"
: "${NETBSD_DIR:?NETBSD_DIR is required (path to NetBSD source/build tree)}"

DOCKER_IMAGE="${DOCKER_IMAGE:-netbsd-build}"
KERNEL="${KERNEL:-GENERIC64}"

if [ ! -f "$XV6_DIR/Makefile" ]; then
  echo "ERROR: $XV6_DIR/Makefile not found — XV6_DIR must point at the ncc repo." >&2
  exit 1
fi
if [ ! -d "$XV6_DIR/tests/netbsd/tools" ]; then
  echo "ERROR: $XV6_DIR/tests/netbsd/tools not found — XV6_DIR must point at a recent ncc checkout." >&2
  exit 1
fi
if [ ! -d "$NETBSD_DIR/src" ] || [ ! -d "$NETBSD_DIR/obj" ] || [ ! -d "$NETBSD_DIR/tooldir" ]; then
  echo "ERROR: NETBSD_DIR=$NETBSD_DIR must contain src/, obj/, and tooldir/." >&2
  exit 1
fi

# The payload that runs inside the container, passed via stdin to avoid all
# quoting nightmares.
docker run --rm -i \
  -e "KERNEL=$KERNEL" \
  -v "$XV6_DIR":/xv6 \
  -v "$NETBSD_DIR":/netbsd \
  "$DOCKER_IMAGE" bash -s <<'PAYLOAD'
set -e
echo "=== Building ncc-linux ==="
cd /xv6
make clean >/dev/null
make CC=gcc CFLAGS="-Wall -std=c11 -g -O2 -Wno-unused-parameter -Wno-switch -D_GNU_SOURCE" >/dev/null 2>&1
ls -l /xv6/ncc

echo "=== Cross-as symlink ==="
ln -sf /netbsd/tooldir/bin/aarch64--netbsd-as /usr/local/bin/aarch64-elf-as

echo "=== Re-pointing build-dir symlinks to Docker paths ==="
cd /netbsd/obj/sys/arch/evbarm/compile/"$KERNEL"
ln -sfn /netbsd/src/sys/arch/evbarm/include machine
ln -sfn /netbsd/src/sys/arch/arm/include arm
ln -sfn /netbsd/src/sys/arch/aarch64/include aarch64
ln -sfn /netbsd/src/sys/arch/evbarm/include evbarm

ls -l /xv6/tests/netbsd/tools/ncc-elf-wrapper.sh

echo "=== Forcing full rebuild: removing .o + linker outputs ==="
# bmake's -B flag is "compat mode", NOT force-rebuild. To genuinely re-exercise
# every codegen path with the current ncc, delete .o's and the linker outputs
# so nbmake has to recompile + relink.  Build state in obj/ is otherwise
# preserved across runs.
find . -maxdepth 1 -name '*.o' -delete
rm -f netbsd netbsd.img netbsd.bin netbsd.gdb netbsd.map "netbsd-$KERNEL.debug"

echo "=== Running kernel build with CC=ncc ==="
# bsd.own.mk:660 unconditionally reassigns CC.  Override TOOL_CC.gcc directly
# (build.sh -V rejects dotted names so we go straight to nbmake).  Wrapper
# adds -target elf + -D__NetBSD__=1 to every ncc invocation.
exec /netbsd/tooldir/bin/nbmake-evbarm64 -j 2 \
  TOOL_CC.gcc=/xv6/tests/netbsd/tools/ncc-elf-wrapper.sh \
  CC=/xv6/tests/netbsd/tools/ncc-elf-wrapper.sh \
  all
PAYLOAD
