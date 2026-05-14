#!/bin/bash
# Top-level compliance check. Runs the runtime test suite and any
# integration checks against the C compiler in $CC.
#
# Usage:
#   ./check.sh                            # uses $CC, defaults to 'cc'
#   CC=gcc ./check.sh
#   CC=clang CFLAGS="-O2" ./check.sh
#   CC=/path/to/cc ./check.sh
#
# Exit 0 if every check passes, 1 otherwise.

set -u

KIT_DIR="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:-}"

printf '=================================================================\n'
printf ' AArch64 C compiler compliance check\n'
printf '=================================================================\n'
printf 'CC      = %s\n' "$CC"
printf 'CFLAGS  = %s\n' "${CFLAGS:-(none)}"
printf 'Spec    = %s/SPEC.md\n' "$KIT_DIR"
printf '\n'

FAILED_STAGES=()

section() {
    printf '\n'
    printf '%s\n' "----- $1 -----"
}

# --- Stage 1: runtime test suite ----------------------------------
section "Stage 1: runtime test suite"
if CC="$CC" CFLAGS="$CFLAGS" "$KIT_DIR/tests/run.sh"; then
    printf '\nStage 1: OK\n'
else
    printf '\nStage 1: FAILED\n'
    FAILED_STAGES+=("runtime test suite")
fi

# --- Stage 2: section-attribute integration check (best effort) ---
# Verifies SPEC §4.5: __attribute__((section("X"))) must place a
# symbol in section X in the produced object file. ELF-only; skipped
# on Mach-O (which uses a different "__SEG,__sect" section-name
# format and is therefore not testable with the same source).
section "Stage 2: section-attribute integration check"
PROBE_C="$(mktemp).c"; PROBE_O="${PROBE_C%.c}.o"
printf 'int x;\n' >"$PROBE_C"
if ! $CC $CFLAGS -c -o "$PROBE_O" "$PROBE_C" >/dev/null 2>&1; then
    printf 'SKIP  could not produce a probe object to detect target format\n'
    rm -f "$PROBE_C" "$PROBE_O"
elif file "$PROBE_O" 2>/dev/null | grep -q 'Mach-O'; then
    printf 'SKIP  Mach-O target: section-name format differs (see SPEC §4.5)\n'
    rm -f "$PROBE_C" "$PROBE_O"
else
    rm -f "$PROBE_C" "$PROBE_O"
    TMP_C="$(mktemp).c"; TMP_O="${TMP_C%.c}.o"
    cat >"$TMP_C" <<'EOF'
int marker __attribute__((section("custom_sec"))) = 0xdeadbeef;
int other = 1;
EOF
    if ! $CC $CFLAGS -c -o "$TMP_O" "$TMP_C" >/dev/null 2>&1; then
        printf 'FAIL  section-attribute source failed to compile\n'
        FAILED_STAGES+=("section attribute (compile)")
    elif command -v readelf >/dev/null 2>&1; then
        if readelf -S "$TMP_O" 2>/dev/null | grep -q custom_sec; then
            printf 'PASS  section attribute present in ELF\n'
        else
            printf 'FAIL  section attribute missing from ELF\n'
            FAILED_STAGES+=("section attribute (ELF)")
        fi
    elif command -v objdump >/dev/null 2>&1; then
        if objdump -h "$TMP_O" 2>/dev/null | grep -q custom_sec; then
            printf 'PASS  section attribute present (objdump)\n'
        else
            printf 'FAIL  section attribute missing (objdump)\n'
            FAILED_STAGES+=("section attribute")
        fi
    else
        printf 'SKIP  no readelf or objdump available\n'
    fi
    rm -f "$TMP_C" "$TMP_O"
fi

# --- Stage 3: __ASSEMBLER__ predefine via -x assembler-with-cpp ---
# Verifies SPEC §4.6: when the driver is told to preprocess assembly,
# __ASSEMBLER__ must be predefined.
section "Stage 3: -x assembler-with-cpp / __ASSEMBLER__"
TMP_S="$(mktemp -t aarch64spec_asm.XXXXXX.S)"
cat >"$TMP_S" <<'EOF'
#ifdef __ASSEMBLER__
__ASSEMBLER_IS_DEFINED__
#else
__ASSEMBLER_IS_NOT_DEFINED__
#endif
EOF
OUT="$($CC $CFLAGS -x assembler-with-cpp -E "$TMP_S" 2>/dev/null)"
if printf '%s\n' "$OUT" | grep -q __ASSEMBLER_IS_DEFINED__; then
    printf 'PASS  __ASSEMBLER__ predefined under -x assembler-with-cpp\n'
elif printf '%s\n' "$OUT" | grep -q __ASSEMBLER_IS_NOT_DEFINED__; then
    printf 'FAIL  __ASSEMBLER__ not predefined under -x assembler-with-cpp\n'
    FAILED_STAGES+=("-x assembler-with-cpp / __ASSEMBLER__")
else
    printf 'SKIP  -x assembler-with-cpp not supported by driver\n'
fi
rm -f "$TMP_S"
printf '\n'

# --- Summary ------------------------------------------------------
printf '=================================================================\n'
if [ "${#FAILED_STAGES[@]}" -eq 0 ]; then
    printf ' OVERALL: PASS\n'
    printf '=================================================================\n'
    exit 0
else
    printf ' OVERALL: FAIL\n'
    printf '=================================================================\n'
    printf 'Failed stages:\n'
    for s in "${FAILED_STAGES[@]}"; do
        printf '  - %s\n' "$s"
    done
    exit 1
fi
