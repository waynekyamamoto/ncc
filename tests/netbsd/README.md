# tests/netbsd — NetBSD/aarch64 kernel built with ncc

Build the NetBSD-10 `evbarm64` kernel with [ncc](../../) for most kernel C
files, and boot it under QEMU. Same flow on Linux and macOS.

## Prereqs

**Linux:**
```bash
sudo apt install build-essential bison flex byacc zlib1g-dev \
                 ca-certificates git curl qemu-system-arm
```

**macOS:** Xcode Command Line Tools (`xcode-select --install`),
[Docker Desktop](https://www.docker.com/products/docker-desktop) running,
and QEMU (`brew install qemu`). Docker is used internally so the cross-
toolchain ends up as Linux ELF and matches the kernel-build container.

## Quickstart — kernel only

```bash
# 0. NetBSD source (one time per machine):
git clone --branch netbsd-10 https://github.com/NetBSD/src.git ~/netbsd/src

# 1. Build ncc itself (one time per checkout):
make
scripts/bootstrap_validate.sh        # produces ./ncc2

# 2. Build the cross-toolchain (one time per machine, ~30 min, gcc-built):
make netbsd-tools

# 3. Build the kernel with ncc (every iteration, ~5–10 min):
make netbsd

# 4. Smoke-test the boot (no disk):
make netbsd-boot
```

Output: `~/netbsd/obj/sys/arch/evbarm/compile/MINIMAL_VIRT64/netbsd.img`.

Overrides: `NETBSD_DIR=/path/to/netbsd`, `NETBSD_KERNEL=GENERIC64`.

## Boot to a login prompt

`make netbsd-boot` only proves the kernel runs to storage probe. To get a
real shell you need a root disk image. Build a minimal one once:

```bash
cd ~/netbsd

# Download the NetBSD 10.1 binary sets:
curl -O https://cdn.netbsd.org/pub/NetBSD/NetBSD-10.1/evbarm-aarch64/binary/sets/base.tar.xz
curl -O https://cdn.netbsd.org/pub/NetBSD/NetBSD-10.1/evbarm-aarch64/binary/sets/etc.tar.xz

# Extract as root (PAM checks /etc/pam.d/login is root-owned):
sudo rm -rf rootfs-staging && sudo mkdir rootfs-staging
sudo tar -xpJf base.tar.xz -C rootfs-staging/
sudo tar -xpJf etc.tar.xz  -C rootfs-staging/

# Minimal rc.conf + fstab:
echo 'rc_configured=YES'              | sudo tee -a rootfs-staging/etc/rc.conf
echo 'postfix=NO'                     | sudo tee -a rootfs-staging/etc/rc.conf
echo '/dev/ld0a / ffs rw,noatime 1 1' | sudo tee    rootfs-staging/etc/fstab

# Pack into an FFS image (nbmakefs must run as root to preserve ownership):
sudo ~/netbsd/tooldir/bin/nbmakefs -t ffs -s 512m -o version=2 \
  ~/netbsd/netbsd-root.img rootfs-staging/
sudo chmod 666 ~/netbsd/netbsd-root.img
```

Then boot:

```bash
qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 -m 512 -smp 4 -nographic \
  -kernel ~/netbsd/obj/sys/arch/evbarm/compile/MINIMAL_VIRT64/netbsd.img \
  -drive file=$HOME/netbsd/netbsd-root.img,if=none,id=hd0,format=raw \
  -device virtio-blk-device,drive=hd0
```

Login: `root`, no password. Exit QEMU: `Ctrl-A x`.

A fancier alternative — 4 GB image with a user account, static networking,
and pkgsrc tools (git/curl/openssl) pre-baked — lives in `build-rootfs.sh`.
Use it if you need network or packages inside the VM.

## What gets built by what

ncc only compiles the kernel's C. Everything else is gcc:

| Layer | Built by |
|---|---|
| Cross-toolchain (`aarch64--netbsd-gcc`, `nbmake-evbarm`, `nbmakefs`, `dbsym`) | host gcc, via NetBSD's `build.sh tools` |
| Kernel C — most files | **ncc** (`-target elf`) |
| Kernel `.S` files + link step + 4 specific C files | cross-gcc |
| Userland (`/bin`, `/sbin`, libc, …) | NetBSD release engineers' gcc — extracted from `base.tar.xz` |

The four C files still routed to gcc, tracked in `tools/ncc-elf-wrapper.sh`:

| File | Why |
|---|---|
| `crypto/chacha/chacha_{ref,impl,selftest}.c` | ncc miscompile — bit-rotation / XOR codegen bug |
| `kern/kern_ksyms_buf.c` | ncc OOMs on the giant `db_symtab[]` initializer |
| `crypto/chacha/arch/arm/{chacha_neon,aes_neon,aes_neon_subr}.c` | NEON intrinsics — gcc/clang extension, not ISO C |

The first two are open ncc bugs. The NEON files stay shunted unless ncc
grows ARM SIMD intrinsic support.

## Single-file compile (debugging)

```bash
NCC=$(pwd)/ncc2 \
NETBSD_SRC=$HOME/netbsd/src \
BUILD_DIR=$HOME/netbsd/obj/sys/arch/evbarm/compile/MINIMAL_VIRT64 \
bash tests/netbsd/tools/ncc-kern.sh -c $HOME/netbsd/src/sys/kern/foo.c -o /tmp/foo.o
```

## Layout

- `build.sh`, `tools/build-tools.sh` — entry points; dispatch on `uname`
- `tools/native-kernel-build.sh`, `tools/docker-kernel-build.sh` — per-platform internals
- `tools/ncc-elf-wrapper.sh` — `CC` wrapper: `-target elf`, gcc shunts, stub substitution
- `tools/boot-test.sh` — QEMU smoke test
- `MINIMAL_VIRT64` — the diagnostic kernel config that boots
- `Dockerfile` — Ubuntu 22.04 build image (used on macOS only)
- `stubs/` — empty `.o`s for SoC drivers and NEON crypto exports
- `build-rootfs.sh` — provisions the 4 GB FFS root disk (orthogonal to `make netbsd`)
- `CHECKPOINT-2026-05-06.md` — current state at the login-prompt milestone
- `STATUS.md`, `reference-boot.log` — running progress log + reference boot

## License

Build glue and stubs in this directory are MIT (the ncc license).
NetBSD kernel sources referenced by the build are BSD 2-clause.
