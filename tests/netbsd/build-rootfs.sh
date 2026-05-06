#!/bin/bash
# build-rootfs.sh — Build the provisioned NetBSD/aarch64 root disk image.
#
# Produces:
#   $NETBSD_DIR/netbsd-new.img   — 4 GB FFS root disk, provisioned with:
#       - git 2.52, curl 8.17, mozilla-rootcerts-openssl (from pkgsrc 10.0_2025Q4)
#       - user wayne (uid 1000, group users+wheel, password foobarbazz)
#       - static networking 10.0.2.15/24 gw 10.0.2.2 (QEMU user-mode NAT)
#       - /etc/resolv.conf: nameserver 10.0.2.3
#       - ~1M inode slots (density=4096)
#
# REQUIREMENTS:
#   - NetBSD source + tooldir + sysroot at $NETBSD_DIR (default ~/netbsd)
#   - NetBSD 10.1 base+etc sets already extracted into rootfs-staging/
#   - qemu-system-aarch64 available
#   - Must run as a user who can sudo (for nbmakefs and QEMU with virtio)
#
# STEP 0: Extract base sets if staging is empty
#   sudo tar -xpf $NETBSD_DIR/base.tar.xz -C rootfs-staging/
#   sudo tar -xpf $NETBSD_DIR/etc.tar.xz  -C rootfs-staging/
#
# NOTE: This script uses the kernel already built at:
#   $NETBSD_DIR/obj/sys/arch/evbarm/compile/MINIMAL_VIRT64/netbsd.img
# Build the kernel first with: bash tests/netbsd/tools/native-kernel-build.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NETBSD_DIR="${NETBSD_DIR:-$HOME/netbsd}"
STAGING="$NETBSD_DIR/rootfs-staging"
OUTPUT="$NETBSD_DIR/netbsd-new.img"
KERNEL="$NETBSD_DIR/obj/sys/arch/evbarm/compile/MINIMAL_VIRT64/netbsd.img"
NBMAKEFS="$NETBSD_DIR/tooldir/bin/nbmakefs"

if [ ! -f "$KERNEL" ]; then
    echo "ERROR: kernel not found at $KERNEL" >&2
    echo "  Run: bash tests/netbsd/tools/native-kernel-build.sh" >&2
    exit 1
fi

echo "=== Building 4GB root image ==="
sudo "$NBMAKEFS" \
    -t ffs \
    -s 4g \
    -o version=2,density=4096,bsize=8192,fsize=1024 \
    "$OUTPUT" \
    "$STAGING"
echo "Image created: $OUTPUT"

echo ""
echo "=== Provisioning: booting for firstboot ==="
echo "This boots once to install packages and add user wayne."
echo "The VM will power off automatically when done."
echo ""

FIRSTBOOT_LOG="$NETBSD_DIR/firstboot.log"
> "$FIRSTBOOT_LOG"

sudo qemu-system-aarch64 \
    -M virt,gic-version=3 \
    -cpu cortex-a72 \
    -m 512 \
    -smp 4 \
    -nographic \
    -kernel "$KERNEL" \
    -drive file="$OUTPUT",if=none,id=hd0,format=raw \
    -device virtio-blk-device,drive=hd0 \
    -netdev user,id=net0 \
    -device virtio-net-device,netdev=net0 \
    2>&1 | tee "$FIRSTBOOT_LOG"

if grep -q '=== firstboot: done ===' "$FIRSTBOOT_LOG"; then
    echo ""
    echo "=== Provisioning successful! ==="
    echo "Image ready: $OUTPUT  ($(du -sh "$OUTPUT" | cut -f1))"
    echo "Kernel:      $KERNEL  ($(du -sh "$KERNEL" | cut -f1))"
    echo ""
    echo "Copy to Mac with:"
    echo "  docker cp <container_id>:$OUTPUT ~/netbsd/netbsd-new.img"
    echo "  docker cp <container_id>:$KERNEL ~/netbsd/netbsd.img"
    echo ""
    echo "Boot on Mac:"
    echo "  qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 -m 512 -smp 4 -nographic \\"
    echo "    -kernel ~/netbsd/netbsd.img \\"
    echo "    -drive file=~/netbsd/netbsd-new.img,if=none,id=hd0,format=raw \\"
    echo "    -device virtio-blk-device,drive=hd0 \\"
    echo "    -netdev user,id=net0 -device virtio-net-pci,netdev=net0"
    echo ""
    echo "Login: wayne / foobarbazz   (or root, no password)"
else
    echo ""
    echo "=== Provisioning FAILED — check $FIRSTBOOT_LOG ===" >&2
    exit 1
fi
