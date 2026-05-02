#!/bin/bash
# Compile a NetBSD/evbarm64 kernel C file with ncc.
#
# Usage:   ncc-kern.sh [extra ncc flags] -c <source.c> -o <output.o>
# Example: ncc-kern.sh -c kern/subr_prf.c -o /tmp/subr_prf.o

set -e

: "${NCC:?NCC is required (path to ncc binary)}"
: "${NETBSD_SRC:?NETBSD_SRC is required (path to NetBSD source tree, e.g. /path/to/netbsd/src)}"
: "${BUILD_DIR:?BUILD_DIR is required (path to evbarm GENERIC64 obj, e.g. /path/to/netbsd/obj/sys/arch/evbarm/compile/GENERIC64)}"

cd "$BUILD_DIR"

exec "$NCC" -target elf -ffreestanding -nostdinc \
    -I. \
    -I"$NETBSD_SRC/sys/external/bsd/libnv/dist" \
    -I"$NETBSD_SRC/sys/external/bsd/acpica/dist" \
    -I"$NETBSD_SRC/sys/../common/lib/libx86emu" \
    -I"$NETBSD_SRC/sys/../common/lib/libc/misc" \
    -I"$NETBSD_SRC/sys/../common/include" \
    -I"$NETBSD_SRC/sys/arch" \
    -I"$NETBSD_SRC/sys" \
    -I"$NETBSD_SRC/sys/lib/libkern/../../../common/lib/libc/quad" \
    -I"$NETBSD_SRC/sys/lib/libkern/../../../common/lib/libc/string" \
    -I"$NETBSD_SRC/sys/lib/libkern/../../../common/lib/libc/arch/aarch64/string" \
    -I"$NETBSD_SRC/sys/lib/libkern/../../../common/lib/libc/arch/aarch64/atomic" \
    -I"$NETBSD_SRC/sys/lib/libkern/../../../common/lib/libc/hash/sha3" \
    -DCOMPAT_UTILS -DAARCH64 -DARM_GENERIC_TODR -DFPU_VFP \
    -D__HAVE_PCI_CONF_HOOK -D__HAVE_PCI_MSI_MSIX \
    -DCOMPAT_44 -DARMV81_PAN -D_KERNEL -D_KERNEL_OPT \
    "$@"
