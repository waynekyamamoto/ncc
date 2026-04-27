# ncc — ARM64/macOS C Compiler

## Branches
- `main` — stable; Linux kernel scan work happens here
- `xv6-aarch64` — xv6 port; ELF output mode for ncc, xv6 kernel compilation, QEMU boot

## Active work
- **main**: Linux kernel subsystem scan (mm/, kernel/, fs/, net/*). Pre-include fix file at `/tmp/ncc_linux_fix.h`. Scan script at `/tmp/ncc_scan.sh`.
- **xv6-aarch64**: Port ncc to emit ELF assembly, compile xv6-aarch64 kernel + user programs with ncc, boot under QEMU. xv6 source at `~/xv6-aarch64` (to be cloned). Cross-toolchain: `aarch64-elf-binutils` (brew).

## bootstrap_validate

Clean build with clang, then confirm the compiler reaches a fixed point when compiling itself:

```bash
# 1. Clean build with clang
make clean && make -j$(sysctl -n hw.ncpu)

# 2. Stage 1: build ncc with itself
mkdir -p stage1 stage2
ln -sf /Users/yamamoto/new_compiler/include stage1/include
ln -sf /Users/yamamoto/new_compiler/include stage2/include
for f in src/*.c; do
  ./ncc -c -o "stage1/$(basename ${f%.c}.o)" "$f"
done
./ncc -o stage1/ncc stage1/*.o

# 3. Stage 2: build ncc with stage1
for f in src/*.c; do
  stage1/ncc -c -o "stage2/$(basename ${f%.c}.o)" "$f"
done
stage1/ncc -o stage2/ncc stage2/*.o
ln -sf stage2/ncc ncc2

# 4. Check fixed point: stage1 == ncc2
if [ "$(md5 -q stage1/ncc)" = "$(md5 -q ncc2)" ]; then
  echo "FIXED POINT: stage1 == ncc2"
else
  echo "MISMATCH: stage1 != ncc2"
fi
```

Note: ncc finds its headers via `_NSGetExecutablePath`, so each stage binary needs
an `include/` symlink next to it pointing at the real include directory.
