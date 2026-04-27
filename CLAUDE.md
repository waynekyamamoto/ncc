# ncc — ARM64/macOS C Compiler

## Branches
- `main` — stable; Linux kernel scan work happens here
- `xv6-aarch64` — xv6 port; ELF output mode for ncc, xv6 kernel compilation, QEMU boot

## Active work
- **main**: Linux kernel subsystem scan (mm/, kernel/, fs/, net/*). Pre-include fix file at `/tmp/ncc_linux_fix.h`. Scan script at `/tmp/ncc_scan.sh`.
- **xv6-aarch64**: Port ncc to emit ELF assembly, compile xv6-aarch64 kernel + user programs with ncc, boot under QEMU. xv6 source at `~/xv6/xv6-aarch64` (to be cloned). Cross-toolchain: `aarch64-elf-binutils` (brew).

## xv6-aarch64 build plan

Goal: compile the xv6 teaching OS for ARM64 using ncc, boot it under QEMU.

### Phase 0 — Prerequisites
```bash
brew install aarch64-elf-binutils   # aarch64-elf-as, aarch64-elf-ld, aarch64-elf-objcopy
brew install qemu                   # already installed (v11.0.0)
git clone https://github.com/k-mrm/xv6-aarch64 ~/xv6/xv6-aarch64
```

### Phase 1 — Reference build with GCC (baseline)
Build xv6 with the standard GCC toolchain first to confirm QEMU boots correctly
before ncc enters the picture.
```bash
cd ~/xv6/xv6-aarch64
brew install aarch64-elf-gcc        # if needed for reference build
make
qemu-system-aarch64 -cpu cortex-a72 -machine virt,gic-version=3 \
  -kernel kernel/kernel -m 128M -smp 4 -nographic \
  -drive file=fs.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
# Should see: xv6 kernel is booting ... $ prompt
```

### Phase 2 — Add ELF output mode to ncc
ncc currently emits Mach-O assembly. xv6 needs ELF. Changes needed in
`src/codegen_arm64.c`:

| Mach-O (current)              | ELF (needed)      |
|-------------------------------|-------------------|
| `.section __TEXT,__text,...`  | `.section .text`  |
| `.section __DATA,__data`      | `.section .data`  |
| `.section __DATA,__bss`       | `.section .bss`   |
| `.section __DATA,__const`     | `.section .rodata`|
| `_foo` (leading underscore)   | `foo`             |
| `.globl _foo`                 | `.globl foo`      |

Add a `-target elf` flag (or `-felf`) to switch codegen to ELF mode.
The ARM64 instructions themselves are identical — only directives change.

### Phase 3 — Compile xv6 kernel C files with ncc
- 25 kernel `.c` files compiled with `ncc -target elf`
- 5 `.S` files (entry, vectors, context switch) assembled with `aarch64-elf-as` directly
- Linked with `aarch64-elf-ld` using `kernel/kernel.ld`

### Phase 4 — Compile xv6 user programs with ncc
- 20 user `.c` files (cat, ls, sh, grep, etc.) + ulib.c compiled with ncc
- Build `fs.img` with xv6's `mkfs`
- Milestone: ncc-compiled programs running on ncc-compiled kernel

### Phase 5 — Docker
- `Dockerfile`: install QEMU, copy kernel + fs.img, boot on `docker run`

### Key facts
- xv6 repo: https://github.com/k-mrm/xv6-aarch64
- QEMU machine: `virt`, CPU `cortex-a72`, GIC v3, 128MB RAM
- Kernel load address: 0x40000000 (PA), 0xffffff8040000000 (VA)
- Kernel: 25 C files, 5 .S files
- User programs: 20 C files, ulib.c, umalloc.c, 1 .S (initcode.S)
- ncc already installed/built: `./ncc` in this repo root

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
