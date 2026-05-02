#!/bin/bash
# Boot a NetBSD/aarch64 kernel image under QEMU and check for known boot-banner
# milestones. Returns 0 if the kernel reaches the "root device:" prompt, 1
# otherwise. Used to gate progress: ncc-built kernel must match gcc baseline.
#
# Usage:   bash tools/boot-test.sh /path/to/netbsd.img
# Default: /Users/yamamoto/netbsd/obj/sys/arch/evbarm/compile/GENERIC64/netbsd.img
#
# The image must be a "Linux ARM64 boot executable" (NetBSD's `.img`, not
# the `.elf` artifact).

set -e

IMG="${1:-/Users/yamamoto/netbsd/obj/sys/arch/evbarm/compile/GENERIC64/netbsd.img}"

if [ ! -f "$IMG" ]; then
    echo "boot-test: kernel image not found: $IMG" >&2
    exit 2
fi

LOG=$(mktemp -t boot-test.XXXX)
trap "rm -f $LOG" EXIT

echo "boot-test: image $IMG" >&2
echo "boot-test: log    $LOG" >&2

# 25-second cap. The reference gcc kernel reaches "root device:" in <2s on
# QEMU/M-series; we just need slack for slower variants.
timeout 25 qemu-system-aarch64 \
    -M virt,gic-version=3 \
    -cpu cortex-a72 \
    -m 512 \
    -smp 4 \
    -nographic \
    -kernel "$IMG" 2>&1 | tee "$LOG" || true

echo
echo "=== boot-test signals ==="

PASS=0
FAIL=0

check() {
    local pat="$1" desc="$2"
    if grep -q "$pat" "$LOG"; then
        echo "  PASS  $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $desc  (no '$pat' in output)"
        FAIL=$((FAIL + 1))
    fi
}

check "NetBSD/evbarm"          "kernel banner"
check "Cortex-A72"             "cpu probe"
check "GICv3"                  "interrupt controller"
check "plcom0: console"        "PL011 UART"
check "virtio"                 "virtio bus enumeration"
check "root device:"           "root prompt reached"

echo
echo "=== summary: $PASS pass, $FAIL fail ==="

# Boot is "successful enough" if we reach the root device prompt.
grep -q "root device:" "$LOG" && exit 0
exit 1
