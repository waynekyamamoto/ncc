# tests/netbsd — NetBSD/aarch64 build with ncc

Build NetBSD-10 kernel for `evbarm64` with [ncc](../../) instead of gcc,
and boot-test it under QEMU.

Same shape as `tests/sqlite/`, `tests/cpython/`, `tests/doom/`: this
directory holds OUR build glue, configs, and stubs. The NetBSD kernel
source itself lives outside the repo (it's 3.6 GB and not ours).

## Status

The ncc-built `MINIMAL_VIRT64` kernel **boots to the `root device:`
prompt** under QEMU virt — Phase 1 milestone reached on 2026-04-30.
See [`STATUS.md`](STATUS.md) for the full build-iteration log + known
issues, and [`reference-boot.log`](reference-boot.log) for the actual
boot output that anchors the milestone.

## Layout

```
tests/netbsd/
├── build.sh              # entry point — build [+ optional boot-test]
├── README.md             # this file
├── STATUS.md             # running progress log (build attempts, fixes, gaps)
├── reference-boot.log    # successful boot output to root device: prompt
├── Dockerfile            # ubuntu:22.04 + build-essential + zlib (NetBSD tools)
├── MINIMAL_VIRT64        # diagnostic kernel config that boots
├── tools/
│   ├── docker-kernel-build.sh   # runs nbmake-evbarm64 inside Docker
│   ├── ncc-elf-wrapper.sh       # CC wrapper: -target elf, stub substitution
│   ├── gcc-elf-wrapper.sh       # gcc reference wrapper (parallel structure)
│   ├── ncc-kern.sh              # single-file kernel compile (for debugging)
│   └── boot-test.sh             # QEMU boot, grep for milestone banners
└── stubs/
    ├── cfattach_stubs.c  # SoC drivers excluded from QEMU virt
    ├── neon_stub.c       # NEON crypto exports (ncc has no NEON intrinsics)
    └── empty_stub.c      # generic empty .o
```

The NetBSD source/build tree lives at `~/netbsd/{src,obj,tooldir}` (or
wherever `$NETBSD_DIR` points). It's a separate checkout of
[NetBSD/src](https://github.com/NetBSD/src) — too big to commit in this
repo, kept outside per the same pattern as `tests/cpython/` (which
references `/tmp/Python-3.12.3`).

The `xv6` name in some scripts (e.g. `XV6_DIR`, `/xv6`) is a legacy
artifact — that path inside Docker bind-mounts the ncc repo, not xv6.

## Quick start

Assuming `~/netbsd/src/`, `~/netbsd/obj/`, and `~/netbsd/tooldir/`
already exist (NetBSD source cloned and `build.sh tools` run once):

```bash
# Build MINIMAL_VIRT64 + boot-test it
./build.sh MINIMAL_VIRT64 boot

# Or the full GENERIC64 (does not boot today; here for reference)
./build.sh GENERIC64
```

`build.sh` auto-detects the ncc repo location, syncs the kernel config
from this directory into the NetBSD source tree's `conf/` dir, builds
the Docker image on first use, and runs the build.

## Initial NetBSD setup (one-time)

If you don't have a NetBSD source/build tree yet:

```bash
mkdir -p ~/netbsd && cd ~/netbsd
git clone --branch netbsd-10 https://github.com/NetBSD/src.git
cd src
./build.sh -j$(sysctl -n hw.ncpu) -O ../obj -T ../tooldir tools
```

(Builds the cross-toolchain — takes ~30 minutes.)

## Single-file compile (for debugging)

```bash
NCC=/path/to/ncc \
NETBSD_SRC=$HOME/netbsd/src \
BUILD_DIR=$HOME/netbsd/obj/sys/arch/evbarm/compile/MINIMAL_VIRT64 \
bash tools/ncc-kern.sh -c $HOME/netbsd/src/sys/kern/some_file.c -o /tmp/some_file.o
```

Useful when isolating a single failing translation unit.

## What the stubs are for

To get past kernel features ncc doesn't support yet, and to drop SoC
drivers irrelevant to QEMU virt:

- **`neon_stub.c`** — empty-body NEON exports (chacha + aes). Risk: if
  QEMU is configured with NEON and the kernel selects it, crypto
  returns garbage. Boot to `root device:` does not depend on crypto.
- **`cfattach_stubs.c`** — 67 cfattach struct stubs + helper-fn stubs
  for SoC drivers (Tegra, Rockchip, Apple, Sunxi, …). Lets the kernel
  link; those drivers obviously don't function on virt.
- **`empty_stub.c`** — empty translation unit. Substituted for files
  ncc OOMs on (`syscalls_autoload.c`) or that contain only NEON code
  we don't want.

## License

Build glue and stubs in this directory are MIT (the ncc license).
NetBSD kernel sources referenced by the build are NetBSD's
(BSD 2-clause).
