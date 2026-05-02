#!/bin/bash
# Build xv6-aarch64 kernel and user programs with ncc, boot under QEMU.
# Phase 3 + 4 of the xv6-aarch64 plan.
#
# Same shape as tests/sqlite/build.sh: this script lives in the ncc repo,
# the upstream xv6 source lives outside the repo (default ~/xv6/xv6-aarch64),
# build artifacts go to a path outside the repo (default ~/xv6/build/xv6_ncc).
#
# Env overrides:
#   XV6_SRC=path     upstream xv6 source (default ~/xv6/xv6-aarch64)
#   XV6_BUILD=path   build output dir    (default ~/xv6/build/xv6_ncc)
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
# Prefer the bootstrapped ncc2 (proves the build survives the self-hosted
# compiler) when one exists; fall back to the host-built ncc.  Override
# with NCC=path.
if [ -n "${NCC:-}" ]; then
  :
elif [ -x "$REPO_DIR/ncc2" ]; then
  NCC="$REPO_DIR/ncc2"
else
  NCC="$REPO_DIR/ncc"
fi
XV6="${XV6_SRC:-$HOME/xv6/xv6-aarch64}"
BUILD="${XV6_BUILD:-$HOME/xv6/build/xv6_ncc}"

if [ ! -d "$XV6" ]; then
  echo "ERROR: xv6-aarch64 not found at $XV6"
  echo "Run: git clone https://github.com/k-mrm/xv6-aarch64 $XV6"
  echo "(or set XV6_SRC=path to an existing checkout)"
  exit 1
fi

mkdir -p "$BUILD/kernel" "$BUILD/user"

# Sync our user-program additions into the xv6 source tree's user/ dir
# so the per-file compile loop picks them up. Idempotent; only files
# we wrote (hanoi.c) live in this repo.
for src in "$SCRIPT_DIR"/*.c; do
  [ -e "$src" ] || continue
  dst="$XV6/user/$(basename "$src")"
  if ! cmp -s "$src" "$dst" 2>/dev/null; then
    echo "Syncing user program: $src -> $dst"
    cp "$src" "$dst"
  fi
done

if [ ! -x "$NCC" ]; then
  echo "=== Building ncc ($NCC missing) ==="
  (cd "$REPO_DIR" && make -j$(sysctl -n hw.ncpu))
else
  echo "=== Using existing ncc: $NCC ==="
fi

echo ""
echo "=== Phase 3: Kernel C files (ncc -target elf) ==="
KFAIL=()
for f in "$XV6"/kernel/*.c; do
  base=$(basename "${f%.c}")
  # ramdisk.c is not linked and has a struct mismatch — skip it
  [ "$base" = "ramdisk" ] && continue
  if "$NCC" -target elf -c -I"$XV6/kernel" -o "$BUILD/kernel/${base}.o" "$f" 2>"$BUILD/kernel/${base}.err"; then
    echo "  OK: $base.c"
  else
    echo "  FAIL: $base.c"
    cat "$BUILD/kernel/${base}.err"
    KFAIL+=("$base")
  fi
done

echo ""
echo "=== Phase 3: Kernel .S files ==="
# entry.S needs C preprocessor for macro expansion
aarch64-elf-gcc -Og -mcpu=cortex-a72 -I"$XV6/kernel" -c \
  -o "$BUILD/kernel/entry.o" "$XV6/kernel/entry.S"
echo "  OK: entry.S (via aarch64-elf-gcc)"

for f in kernelvec.S swtch.S trapasm.S uservec.S; do
  aarch64-elf-as -mcpu=cortex-a72 -I"$XV6/kernel" \
    -o "$BUILD/kernel/${f%.S}.o" "$XV6/kernel/$f" 2>&1
  echo "  OK: $f"
done

echo ""
echo "=== Phase 3: Link kernel ==="
aarch64-elf-ld -z max-page-size=4096 -T "$XV6/kernel/kernel.ld" \
  -o "$BUILD/kernel/kernel" \
  "$BUILD/kernel/entry.o" \
  "$BUILD/kernel/start.o" \
  "$BUILD/kernel/console.o" \
  "$BUILD/kernel/printf.o" \
  "$BUILD/kernel/uart.o" \
  "$BUILD/kernel/kalloc.o" \
  "$BUILD/kernel/spinlock.o" \
  "$BUILD/kernel/string.o" \
  "$BUILD/kernel/main.o" \
  "$BUILD/kernel/vm.o" \
  "$BUILD/kernel/proc.o" \
  "$BUILD/kernel/swtch.o" \
  "$BUILD/kernel/trap.o" \
  "$BUILD/kernel/syscall.o" \
  "$BUILD/kernel/sysproc.o" \
  "$BUILD/kernel/bio.o" \
  "$BUILD/kernel/fs.o" \
  "$BUILD/kernel/log.o" \
  "$BUILD/kernel/sleeplock.o" \
  "$BUILD/kernel/file.o" \
  "$BUILD/kernel/pipe.o" \
  "$BUILD/kernel/exec.o" \
  "$BUILD/kernel/sysfile.o" \
  "$BUILD/kernel/trapasm.o" \
  "$BUILD/kernel/timer.o" \
  "$BUILD/kernel/virtio_disk.o" \
  "$BUILD/kernel/gicv3.o"
echo "  OK: kernel"

echo ""
echo "=== Phase 4: User C files (ncc -target elf) ==="
for f in "$XV6"/user/*.c; do
  base=$(basename "${f%.c}")
  if "$NCC" -target elf -c -I"$XV6" -o "$BUILD/user/${base}.o" "$f" 2>"$BUILD/user/${base}.err"; then
    echo "  OK: $base.c"
  else
    echo "  FAIL: $base.c"
    cat "$BUILD/user/${base}.err"
  fi
done

echo ""
echo "=== Phase 4: User .S files ==="
perl "$XV6/user/usys.pl" > "$BUILD/user/usys.S"
aarch64-elf-gcc -mcpu=cortex-a72 -I"$XV6" -c -o "$BUILD/user/usys.o" "$BUILD/user/usys.S"
aarch64-elf-gcc -mcpu=cortex-a72 -I"$XV6" -I"$XV6/kernel" -nostdinc -c \
  -o "$BUILD/user/initcode.o" "$XV6/user/initcode.S"
echo "  OK: usys.S, initcode.S"

echo ""
echo "=== Phase 4: Link user programs ==="
ULIB=("$BUILD/user/ulib.o" "$BUILD/user/usys.o" "$BUILD/user/printf.o" "$BUILD/user/umalloc.o")
for prog in cat echo grep init kill ln ls mkdir rm sh stressfs wc zombie grind hanoi; do
  aarch64-elf-ld -z max-page-size=4096 -N -e main -Ttext 0 \
    -o "$BUILD/user/_${prog}" "$BUILD/user/${prog}.o" "${ULIB[@]}" 2>/dev/null
  echo "  OK: _$prog"
done
aarch64-elf-ld -z max-page-size=4096 -N -e main -Ttext 0 \
  -o "$BUILD/user/_forktest" "$BUILD/user/forktest.o" \
  "$BUILD/user/ulib.o" "$BUILD/user/usys.o" 2>/dev/null
echo "  OK: _forktest"

# Build initcode binary
aarch64-elf-ld -z max-page-size=4096 -N -e start -Ttext 0 \
  -o "$BUILD/user/initcode.out" "$BUILD/user/initcode.o" 2>/dev/null
aarch64-elf-objcopy -S -O binary "$BUILD/user/initcode.out" "$BUILD/user/initcode"

echo ""
echo "=== Phase 4: Build mkfs (host tool, also via ncc) ==="
# mkfs is a host-side tool that constructs the xv6 filesystem image. ncc
# without -target elf produces a Mach-O binary that runs on macOS — same
# compiler, different output format. Building it with ncc keeps the goal
# of "every C file in the xv6 build goes through ncc" intact.
if [ ! -x "$XV6/mkfs/mkfs" ]; then
  # -I. forces the include resolver to look for "kernel/types.h" relative
  # to the cwd before consulting the macOS SDK's `kernel` framework.
  (cd "$XV6" && "$NCC" -I. -o mkfs/mkfs mkfs/mkfs.c)
fi
echo "  OK: mkfs"

echo ""
echo "=== Phase 4: Build fs.img ==="
# mkfs needs binaries in a directory without leading path components
cp "$BUILD/user"/_* "$XV6/user/"
(cd "$XV6" && mkfs/mkfs "$BUILD/fs.img" README \
  user/_cat user/_echo user/_forktest user/_grep user/_init \
  user/_kill user/_ln user/_ls user/_mkdir user/_rm user/_sh \
  user/_stressfs user/_grind user/_wc user/_zombie user/_hanoi)
echo "  OK: fs.img"

echo ""
if [ ${#KFAIL[@]} -eq 0 ]; then
  echo "SUCCESS: ncc-compiled xv6 kernel + user programs"
  echo ""
  echo "Boot with:"
  echo "  qemu-system-aarch64 -cpu cortex-a72 -machine virt,gic-version=3 \\"
  echo "    -kernel $BUILD/kernel/kernel -m 128M -smp 4 -nographic \\"
  echo "    -drive file=$BUILD/fs.img,if=none,format=raw,id=x0 \\"
  echo "    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0"
else
  echo "PARTIAL: ${#KFAIL[@]} kernel compile failures: ${KFAIL[*]}"
fi
