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
for a in "$@"; do
  case "$a" in -c) HAS_C=1; break ;; esac
done
if [ "$HAS_C" -eq 0 ]; then
  exec /netbsd/tooldir/bin/aarch64--netbsd-gcc "$@"
fi

# Assembly source goes to the cross-gcc.
case "$SRC" in
  *.S)
    exec /netbsd/tooldir/bin/aarch64--netbsd-gcc "$@"
    ;;
  */kern/kern_ksyms_buf.c)
    # SYMTAB_SPACE allocates a giant uninitialized array sized for the
    # kernel's debug-symbol table; ncc OOMs on the resulting big static
    # array initializer.  gcc handles it fine, so route this one file to
    # gcc.  Use the dbsym -P-computed value (the default in nbmake's
    # recipe) — DO NOT override.  An earlier version of this wrapper
    # forced SYMTAB_SPACE=200000000, which inflated kernel `.data` by
    # ~200 MB and pushed globals (notably timehands `th0`) past the
    # early-MMU mapping window.
    exec /netbsd/tooldir/bin/aarch64--netbsd-gcc "$@"
    ;;
  */kern/tty.c)
    # KNOWN ncc MISCOMPILE (2026-05-02): when ncc compiles tty.c, every
    # tty ioctl (TIOCSFLAGS), fcntl(O_NONBLOCK), and open() on /dev/constty
    # returns ENOSYS — getty fails to spawn, login is unreachable.  Bisected
    # by routing single files to gcc (v5..v8 in build logs from 2026-05-02);
    # tty.c alone reproduces, none of {tty_conf.c, subr_devsw.c, spec_vnops.c,
    # vfs_vnops.c, plcom.c} reproduces.  Routing tty.c to gcc lets the
    # ncc-built kernel reach login.  Remove this entry once the codegen bug
    # in ncc/src/ is identified and fixed (likely a function-pointer table
    # initializer in tty.c — the symptom shape matches NULL d_ioctl/d_fcntl
    # slots in a cdevsw or fileops table).
    exec /netbsd/tooldir/bin/aarch64--netbsd-gcc "$@"
    ;;
  */kern/kern_turnstile.c)
    # KNOWN ncc MISCOMPILE (2026-05-03): forcing root=dk1 panics with NULL
    # deref at l->l_syncobj inside lwp_lendpri (compiled standalone from
    # kern_turnstile.c since lwp_lendpri is `static __inline` in lwp.h).
    # Routing this TU to gcc lets the kernel mount dk1 and reach
    # /etc/rc / login.
    #
    # Bisected 2026-05-03: clean diagnostic route — gcc-routed kern_turnstile
    # boots past dk1 mount and reaches /etc/rc; ncc-built reproducibly
    # panics with `Translation Fault L0 ... for 0x18` at PC 0xffffc...f70c
    # (load chain: l + 0x168 → +0x18, faulting because l->l_syncobj == 0).
    # Source-side analysis ruled out lwp init bugs: lwp0 static init, every
    # lwp_create runtime path, and all 4 sleepq writers all assign valid
    # syncobj pointers.  So the bug is in ncc's codegen of kern_turnstile.c
    # itself — likely register-save/spill interaction in turnstile_unlendpri
    # or turnstile_lendpri's call to lwp_lendpri.  Specific instruction
    # mis-emission TBD; needs codegen-side investigation in src/codegen_arm64.c.
    exec /netbsd/tooldir/bin/aarch64--netbsd-gcc "$@"
    ;;
esac

case "$SRC" in
  */chacha_neon.c)
    # Substitute the combined NEON stub (defines all chacha+aes neon symbols).
    new_args=""
    for a in "$@"; do
      if [ "$a" = "$SRC" ]; then new_args="$new_args /xv6/tests/netbsd/stubs/neon_stub.c"; else new_args="$new_args $a"; fi
    done
    exec /xv6/ncc -target elf -D__NetBSD__=1 $new_args
    ;;
  */aes_neon_subr.c)
    # aes_neon_subr.c uses NEON intrinsics — stub it. The substituted file
    # provides the aes_neon_* public API + cfattach + misc null symbols
    # for SoC drivers excluded from compilation.
    new_args=""
    for a in "$@"; do
      if [ "$a" = "$SRC" ]; then new_args="$new_args /xv6/tests/netbsd/stubs/cfattach_stubs.c"; else new_args="$new_args $a"; fi
    done
    exec /xv6/ncc -target elf -D__NetBSD__=1 $new_args
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
        new_args="$new_args /xv6/tests/netbsd/stubs/empty_stub.c"
      else
        new_args="$new_args $a"
      fi
    done
    exec /xv6/ncc -target elf -D__NetBSD__=1 $new_args
    ;;
esac

exec /xv6/ncc -target elf -D__NetBSD__=1 "$@"
