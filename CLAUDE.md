# ncc — ARM64/macOS C Compiler

## Branches
- `main` — all active compiler work and OS-port test corpora live here
- `xv6-aarch64` — historical: original xv6 port branch. Compiler features merged to `main`; xv6 build glue moved to `tests/xv6/`.
- `swap-out` — chibicc swap-out effort, owned by a separate agent (see memory). Do not touch from `~/ncc`.

## Active work
- **NetBSD/aarch64 boot**: get past the `root device:` prompt to userland. Build glue + kernel config + status doc in `tests/netbsd/` (entry: `tests/netbsd/build.sh MINIMAL_VIRT64 boot`). NetBSD source lives at `~/netbsd/src/` (not in this repo).
- **xv6-aarch64**: suspended. Kernel + user programs compile and boot. Build glue in `tests/xv6/`.
- **Linux kernel scan**: suspended. Per-subsystem compile-coverage scan in `tests/linux/` (entry: `tests/linux/scan.sh`). Linux source at `~/ncc-linux/linux/` (not in this repo).

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
make TOOLPREFIX=aarch64-elf-        # Homebrew uses aarch64-elf- prefix
qemu-system-aarch64 -cpu cortex-a72 -machine virt,gic-version=3 \
  -kernel kernel/kernel -m 128M -smp 4 -nographic \
  -drive file=fs.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
# Should see: xv6 kernel is booting ... $ prompt
```
Notes for GCC 15 compat (already patched in xv6-aarch64/Makefile):
- `user/sh.c`: add `-Wno-infinite-recursion` (runcmd tail-calls exec, not truly infinite)
- `user/usertests.c`: add `-Wno-incompatible-pointer-types` (old-style `()` decl vs fn ptr)

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

Clean build with clang, then confirm the compiler reaches a fixed point
when compiling itself:

```bash
scripts/bootstrap_validate.sh
```

The script builds ncc with clang, builds ncc with itself (stage1),
builds ncc with stage1 (stage2), and checks `md5(stage1) == md5(stage2)`.
Output ends with `FIXED POINT: stage1 == ncc2` on success.

Note: ncc finds its headers via `_NSGetExecutablePath`, so each stage
binary needs an `include/` symlink next to it pointing at the real
include directory (the script handles this).
