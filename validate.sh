#!/bin/bash
# validate.sh — run ncc's correctness pyramid.
#
# Tiers:
#   fast   (default, ~3 min):  compliance suite + bootstrap fixed point
#   full   (~5-10 min):        also builds DOOM with ncc2
#   kernel (~50 min, needs Docker + NetBSD source tree):
#                              also builds & boots NetBSD/aarch64
#                              under QEMU via the netbsd-port repo
#
# A new ncc passes the rewrite bar iff it passes the `kernel` tier.
# Day-to-day during a rewrite, `fast` is usually enough.
#
# Usage:
#   ./validate.sh           # fast tier
#   ./validate.sh full      # fast + DOOM
#   ./validate.sh kernel    # fast + DOOM + kernel boot
#
# Env:
#   NETBSD_PORT_DIR  — path to a clone of waynekyamamoto/netbsd-port
#                      (only needed for the `kernel` tier).
#   XV6_DIR          — path to this repo (auto-detected from $0).
#   NETBSD_DIR       — path to NetBSD source/build tree
#                      (only needed for the `kernel` tier).

# Don't `set -e`: we manually track PASS/FAIL counts, and several command
# substitutions below use `grep -c` which returns nonzero when count is 0.

TIER="${1:-fast}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
PASS=0
FAIL=0
START=$(date +%s)

step() { echo; echo "=== $* ==="; }

run() {
    local name="$1"; shift
    if "$@"; then
        echo "  PASS: $name"
        PASS=$((PASS+1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL+1))
    fi
}

step "Building ncc with clang"
(cd "$ROOT" && make clean >/dev/null && make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null 2>&1)
echo "  ncc: $(file "$ROOT/ncc" | head -1)"

step "Compliance suite"
(cd "$ROOT/tests/compliance" && bash run.sh > /tmp/validate-compliance.log 2>&1) || true
COMP_PASS=$(grep -c "^PASS:" /tmp/validate-compliance.log)
COMP_FAIL=$(grep -c "^FAIL:" /tmp/validate-compliance.log)
COMP_SKIP=$(grep -c "^SKIP:" /tmp/validate-compliance.log)
echo "  compliance: $COMP_PASS pass, $COMP_FAIL fail, $COMP_SKIP skip"
if [ "$COMP_FAIL" -eq 0 ]; then
    PASS=$((PASS+1))
else
    FAIL=$((FAIL+1))
    echo "  see /tmp/validate-compliance.log"
fi

step "Bootstrap fixed point"
(cd "$ROOT" && rm -rf stage1 stage2 && mkdir stage1 stage2 \
    && ln -sf "$ROOT/include" stage1/include \
    && ln -sf "$ROOT/include" stage2/include)
BOOT_OK=1
for f in "$ROOT"/src/*.c; do
    if ! "$ROOT/ncc" -c -o "$ROOT/stage1/$(basename "${f%.c}.o")" "$f" 2>/dev/null; then
        echo "  STAGE1 compile FAIL: $f"
        BOOT_OK=0; break
    fi
done
if [ "$BOOT_OK" -eq 1 ]; then
    "$ROOT/ncc" -o "$ROOT/stage1/ncc" "$ROOT"/stage1/*.o 2>/dev/null || BOOT_OK=0
fi
if [ "$BOOT_OK" -eq 1 ]; then
    for f in "$ROOT"/src/*.c; do
        if ! "$ROOT/stage1/ncc" -c -o "$ROOT/stage2/$(basename "${f%.c}.o")" "$f" 2>/dev/null; then
            echo "  STAGE2 compile FAIL: $f"
            BOOT_OK=0; break
        fi
    done
fi
if [ "$BOOT_OK" -eq 1 ]; then
    "$ROOT/stage1/ncc" -o "$ROOT/stage2/ncc" "$ROOT"/stage2/*.o 2>/dev/null || BOOT_OK=0
fi
if [ "$BOOT_OK" -eq 1 ]; then
    S1=$(md5 -q "$ROOT/stage1/ncc")
    S2=$(md5 -q "$ROOT/stage2/ncc")
    if [ "$S1" = "$S2" ]; then
        echo "  bootstrap fixed point OK ($S1)"
        PASS=$((PASS+1))
    else
        echo "  MISMATCH: stage1 $S1 vs stage2 $S2"
        FAIL=$((FAIL+1))
    fi
else
    echo "  bootstrap build failed"
    FAIL=$((FAIL+1))
fi

if [ "$TIER" = "full" ] || [ "$TIER" = "kernel" ]; then
    step "DOOM build with ncc2"
    if (cd "$ROOT" && bash build_doom_ncc2.sh > /tmp/validate-doom.log 2>&1); then
        if [ -x "$ROOT/build/doom_ncc2/doom" ]; then
            DOOM_OK=$(grep -c "^  OK:" /tmp/validate-doom.log)
            DOOM_FAIL=$(grep -c "^  FAIL:" /tmp/validate-doom.log)
            echo "  doom: $DOOM_OK ok, $DOOM_FAIL fail; binary $(file "$ROOT/build/doom_ncc2/doom" | head -1 | cut -d: -f2)"
            if [ "$DOOM_FAIL" -eq 0 ]; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi
        else
            echo "  doom binary not produced"
            FAIL=$((FAIL+1))
        fi
    else
        echo "  doom build script returned nonzero; see /tmp/validate-doom.log"
        FAIL=$((FAIL+1))
    fi
fi

if [ "$TIER" = "kernel" ]; then
    step "NetBSD/aarch64 kernel build + boot"
    if [ -z "$NETBSD_PORT_DIR" ] || [ ! -d "$NETBSD_PORT_DIR" ]; then
        echo "  SKIP: NETBSD_PORT_DIR not set or not found"
    elif [ -z "$NETBSD_DIR" ] || [ ! -d "$NETBSD_DIR" ]; then
        echo "  SKIP: NETBSD_DIR not set or not found"
    else
        # Make sure tools/ symlinked into XV6 so Docker bind-mount sees them.
        [ -L "$ROOT/tools" ] || [ -d "$ROOT/tools" ] || ln -s "$NETBSD_PORT_DIR/tools" "$ROOT/tools"
        if XV6_DIR="$ROOT" NETBSD_DIR="$NETBSD_DIR" \
           bash "$NETBSD_PORT_DIR/tools/docker-kernel-build.sh" > /tmp/validate-kernel.log 2>&1; then
            IMG="$NETBSD_DIR/obj/sys/arch/evbarm/compile/MINIMAL_VIRT64/netbsd.img"
            if [ ! -f "$IMG" ]; then
                IMG="$NETBSD_DIR/obj/sys/arch/evbarm/compile/GENERIC64/netbsd.img"
            fi
            if [ -f "$IMG" ]; then
                # Boot test — reach `root device:` prompt
                if timeout 30 qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 \
                       -m 512 -smp 4 -nographic -kernel "$IMG" 2>&1 \
                       | tee /tmp/validate-kernel-boot.log \
                       | grep -q "root device:"; then
                    echo "  kernel boot: reached root device: prompt"
                    PASS=$((PASS+1))
                else
                    echo "  kernel boot: did not reach root device: prompt"
                    FAIL=$((FAIL+1))
                fi
            else
                echo "  kernel image not found at $IMG"
                FAIL=$((FAIL+1))
            fi
        else
            echo "  kernel build failed; see /tmp/validate-kernel.log"
            FAIL=$((FAIL+1))
        fi
    fi
fi

ELAPSED=$(($(date +%s) - START))

echo
echo "=== summary: $PASS pass, $FAIL fail, ${ELAPSED}s elapsed ==="
if [ "$FAIL" -eq 0 ]; then
    echo "OK"
    exit 0
else
    echo "FAIL"
    exit 1
fi
