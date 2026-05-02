#!/bin/sh
# gcc-elf-wrapper.sh — same substitution logic as ncc-elf-wrapper.sh but
# invokes the cross-gcc directly.  Used to build a gcc reference kernel
# that's apples-to-apples comparable with the ncc-built one (same SoC
# stubs, same NEON crypto stubs, so any boot-behaviour difference is
# attributable to ncc codegen, not to a different file set).

SRC=""
for a in "$@"; do
  case "$a" in
    *.c|*.S) SRC="$a" ;;
  esac
done

case "$SRC" in
  */chacha_neon.c)
    new_args=""
    for a in "$@"; do
      if [ "$a" = "$SRC" ]; then new_args="$new_args /xv6/tests/netbsd/stubs/neon_stub.c"; else new_args="$new_args $a"; fi
    done
    exec /netbsd/tooldir/bin/aarch64--netbsd-gcc $new_args -Wno-error
    ;;
  */aes_neon_subr.c)
    new_args=""
    for a in "$@"; do
      if [ "$a" = "$SRC" ]; then new_args="$new_args /xv6/tests/netbsd/stubs/cfattach_stubs.c"; else new_args="$new_args $a"; fi
    done
    exec /netbsd/tooldir/bin/aarch64--netbsd-gcc $new_args -Wno-error
    ;;
  */aes_neon.c|\
  */aarch64/netbsd32_syscall.c|\
  */kern/kern_sdt.c|\
  */arch/arm/rockchip/*.c|\
  */arch/arm/broadcom/*.c|\
  */arch/arm/amlogic/*.c|\
  */arch/arm/samsung/*.c|\
  */arch/arm/nvidia/*.c|\
  */arch/arm/sunxi/*.c|\
  */arch/arm/altera/*.c|\
  */arch/arm/imx/*.c|\
  */arch/arm/marvell/*.c|\
  */arch/arm/mediatek/*.c|\
  */arch/arm/ti/*.c|\
  */arch/arm/allwinner/*.c|\
  */arch/arm/apple/*.c|\
  */arch/arm/sociomedia/*.c|\
  */kern/syscalls_autoload.c)
    new_args=""
    for a in "$@"; do
      if [ "$a" = "$SRC" ]; then
        new_args="$new_args /xv6/tests/netbsd/stubs/empty_stub.c"
      else
        new_args="$new_args $a"
      fi
    done
    exec /netbsd/tooldir/bin/aarch64--netbsd-gcc $new_args -Wno-error
    ;;
esac

# Demote warnings to non-fatal: ncc is more permissive than gcc, and
# kernel sources rely on that.  Building with ncc-permissive semantics
# under gcc keeps the apples-to-apples comparison meaningful.
exec /netbsd/tooldir/bin/aarch64--netbsd-gcc "$@" -Wno-error
