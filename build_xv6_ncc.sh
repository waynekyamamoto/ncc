#!/bin/bash
# Build xv6-aarch64 kernel and user programs with ncc, boot under QEMU.
# Phase 3 + 4 of the xv6-aarch64 plan.
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
NCC="$ROOT/ncc"
XV6="$ROOT/xv6-aarch64"
BUILD="$ROOT/build/xv6_ncc"

if [ ! -d "$XV6" ]; then
  echo "ERROR: xv6-aarch64 not found at $XV6"
  echo "Run: git clone https://github.com/k-mrm/xv6-aarch64 $XV6"
  exit 1
fi

mkdir -p "$BUILD/kernel" "$BUILD/user"

echo "=== Building ncc ==="
(cd "$ROOT" && make -j$(sysctl -n hw.ncpu))

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
for prog in cat echo grep init kill ln ls mkdir rm sh stressfs wc zombie grind; do
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
echo "=== Phase 4: Build fs.img ==="
# mkfs needs binaries in a directory without leading path components
cp "$BUILD/user"/_* "$XV6/user/"
(cd "$XV6" && mkfs/mkfs "$BUILD/fs.img" README \
  user/_cat user/_echo user/_forktest user/_grep user/_init \
  user/_kill user/_ln user/_ls user/_mkdir user/_rm user/_sh \
  user/_stressfs user/_grind user/_wc user/_zombie)
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
