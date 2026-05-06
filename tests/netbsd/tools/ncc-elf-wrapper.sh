#!/bin/sh
# ncc wrapper for NetBSD/aarch64 cross-compilation. Invoked by nbmake as the CC.
# Adds -target elf (ELF asm output) and -D__NetBSD__=1 (NetBSD's gcc predefines
# this automatically; our generic ncc does not).
#
# Special handling:
# - .S (preprocessed asm) files: ncc does not consume asm; pass through to the
#   cross-gcc which runs cpp+as.
# - NEON crypto source files: stubs (ncc lacks NEON intrinsics).
# - SoC-specific drivers irrelevant to QEMU virt: empty stubs to dodge OOM.

# Find the input source path (last non-flag arg ending in .c or .S).
SRC=""
OUT=""
prev=""
for a in "$@"; do
  case "$prev" in
    -o) OUT="$a" ;;
  esac
  case "$a" in
    *.c|*.S) SRC="$a" ;;
  esac
  prev="$a"
done

# Detect link mode (no -c) — partial-link (-r), or full link of objects.
# ncc's link path is hard-coded for macOS Mach-O; in ELF/cross mode hand off
# to the cross gcc which knows how to invoke the cross ld.
HAS_C=0
HAS_KERNEL=0
for a in "$@"; do
  case "$a" in
    -c) HAS_C=1 ;;
    -D_KERNEL) HAS_KERNEL=1 ;;
  esac
done
NCC_REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
NCC="$NCC_REPO/ncc2"
NETBSD_TOOLDIR="${NETBSD_DIR:-$HOME/netbsd}/tooldir"
CROSS_GCC="$NETBSD_TOOLDIR/bin/aarch64--netbsd-gcc"
STUBS="$NCC_REPO/tests/netbsd/stubs"
# Kernel compiles with +nofp — suppress VR register saves to avoid
# undefined-instruction traps when d0-d7 are saved in variadic prologues.
NCC_EXTRA=""
if [ "$HAS_KERNEL" -eq 1 ]; then NCC_EXTRA="-no-fp-varargs"; fi

if [ "$HAS_C" -eq 0 ]; then
  exec "$CROSS_GCC" "$@"
fi

# Preprocessed assembly (.S) files: the kernel's lib/kern atomics use
# #include "__aarch64_lse.S" which relies on nbmake's VPATH to find the
# source file — ncc's preprocessor doesn't have that context.  The
# cross-gcc handles these correctly, so route all .S through it.
case "$SRC" in
  *.S) exec "$CROSS_GCC" "$@" ;;
esac

case "$SRC" in
  */kern/kern_ksyms_buf.c)
    # SYMTAB_SPACE allocates a giant uninitialized array sized for the
    # kernel's debug-symbol table; ncc OOMs on the resulting big static
    # array initializer.  gcc handles it fine, so route this one file to
    # gcc.  Use the dbsym -P-computed value (the default in nbmake's
    # recipe) — DO NOT override.  An earlier version of this wrapper
    # forced SYMTAB_SPACE=200000000, which inflated kernel `.data` by
    # ~200 MB and pushed globals (notably timehands `th0`) past the
    # early-MMU mapping window.
    exec "$CROSS_GCC" "$@"
    ;;
  */chacha_ref.c|\
  */chacha_impl.c|\
  */chacha_selftest.c)
    # ncc miscompiles chacha (bit-rotation/XOR codegen bug).
    # Route all three to gcc to avoid flooding boot with hexdumps.
    exec "$CROSS_GCC" "$@"
    ;;
esac

case "$SRC" in
  */chacha_neon.c)
    # chacha_neon.c uses ARM NEON intrinsics — ncc can't compile them.
    # Route to cross-gcc which handles the intrinsics correctly.  The kernel
    # build system passes -march=armv8-a (last of three -march flags) to
    # override the global +nofp+nosimd, giving the NEON code its needed ISA.
    exec "$CROSS_GCC" "$@"
    ;;
  */aes_neon_subr.c)
    # aes_neon_subr.c uses NEON intrinsics — stub it. The substituted file
    # provides the aes_neon_* public API + cfattach + misc null symbols
    # for SoC drivers excluded from compilation.
    new_args=""
    for a in "$@"; do
      if [ "$a" = "$SRC" ]; then new_args="$new_args $STUBS/cfattach_stubs.c"; else new_args="$new_args $a"; fi
    done
    exec "$NCC" -target elf -D__NetBSD__=1 $NCC_EXTRA $new_args
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
    # Empty .o so symbols don't multiply-define with neon_stub.c.
    new_args=""
    for a in "$@"; do
      if [ "$a" = "$SRC" ]; then
        new_args="$new_args $STUBS/empty_stub.c"
      else
        new_args="$new_args $a"
      fi
    done
    exec "$NCC" -target elf -D__NetBSD__=1 $NCC_EXTRA $new_args
    ;;
esac

exec "$NCC" -target elf -D__NetBSD__=1 $NCC_EXTRA "$@"
