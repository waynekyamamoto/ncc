# tests/xv6 — xv6-aarch64 build with ncc

Build the [xv6-aarch64](https://github.com/k-mrm/xv6-aarch64) teaching
OS (kernel + user programs) with ncc, boot under QEMU.

**Status:** xv6 work is suspended as of 2026-05-01 (focus shifted to
NetBSD). This directory holds the build script and Dockerfile so the
work isn't lost; it can be reactivated by re-running `build.sh`.

## Layout

```
tests/xv6/
├── README.md     # this file
├── build.sh      # entry point — builds kernel + user + fs.img
└── Dockerfile    # ubuntu:22.04 + qemu-system-arm to run the result
```

## Quick start

```bash
# One-time: clone the upstream xv6 source somewhere
git clone https://github.com/k-mrm/xv6-aarch64 ~/xv6/xv6-aarch64

# Build (default: ncc2 if it exists, otherwise ncc)
./build.sh

# Boot directly via QEMU
qemu-system-aarch64 -cpu cortex-a72 -machine virt,gic-version=3 \
  -kernel ~/xv6/build/xv6_ncc/kernel/kernel -m 128M -smp 4 -nographic \
  -drive file=~/xv6/build/xv6_ncc/fs.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
```

At the `$` prompt, try `hanoi 4` (or any of the standard xv6 commands:
ls, cat README, etc.).

## Boot inside Docker

The `Dockerfile` bundles the built kernel + `fs.img` with QEMU into a
container so you can boot xv6 with a single `docker run`:

```bash
docker build -f tests/xv6/Dockerfile -t xv6-ncc ~/xv6/
docker run --rm -i xv6-ncc
```

(The build context is `~/xv6/` because that's where the build artifacts
live — `build/xv6_ncc/{kernel,fs.img}`.)

## Env overrides

- `XV6_SRC=path` — upstream xv6 source (default `~/xv6/xv6-aarch64`)
- `XV6_BUILD=path` — build output dir (default `~/xv6/build/xv6_ncc`)

The xv6 source tree and build artifacts live outside this repo (same
pattern as `tests/netbsd/` for NetBSD source and `tests/cpython/` for
the CPython source).

## Prerequisites

- `aarch64-elf-binutils` (Homebrew: `brew install aarch64-elf-binutils`)
- `aarch64-elf-gcc` for the few `.S` files (Homebrew: `brew install aarch64-elf-gcc`)
- `qemu-system-aarch64` for booting

The Dockerfile bundles a built kernel + fs.img with QEMU for a
docker-run boot demo.
