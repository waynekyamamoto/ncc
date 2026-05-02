# tests/linux — Linux kernel scan with ncc

Per-file compile-coverage of the Linux kernel using ncc — *not* a full
kernel build. The scan compiles each `.c` in a subsystem with
`ncc -S -o /dev/null` and reports PASS/FAIL/SKIP. The kernel is used
as a high-volume real-world C corpus to flush out compiler bugs.

**Status:** Linux scan is **suspended** as of 2026-05-01 (focus shifted
to NetBSD boot work). This directory holds the scan infrastructure so
it can be reactivated cleanly when Linux work resumes.

## Layout

```
tests/linux/
├── README.md           # this file
├── scan.sh             # entry point — scan one subsystem
├── linux_fix.h         # pre-included header: CONFIGs and stubs
├── skip_list.txt       # files we deliberately don't compile (~165 entries)
└── stubs/
    ├── asm/{kvm_para,static_call}.h
    ├── generated/{vdso-offsets,vdso32-offsets}.h
    └── linux/version.h
```

The Linux source itself lives at `~/ncc-linux/linux/` (Linux 6.1 LTS,
~3.6 GB) — outside this repo, same pattern as `tests/netbsd/` for
NetBSD source and `tests/cpython/` for CPython source.

## Quick start

```bash
# Scan a subsystem
./scan.sh fs-btrfs ~/ncc-linux/linux/fs/btrfs

# With a custom Linux source path
./scan.sh --linux /path/to/other/linux mm /path/to/other/linux/mm
```

Output: per-file `PASS:` / `FAIL:` / `SKIP:` lines plus a final
summary count.

## Env overrides

- `NCC=path` — ncc binary (default: `../../ncc` relative to this script)
- `LINUX=path` — Linux source root (default: `~/ncc-linux/linux`)
- `FIX_HEADER=path` — pre-include header (default: `./linux_fix.h`)
- `STUBS_DIR=path` — stubs include dir (default: `./stubs`)
- `SKIP_LIST=path` — skip list (default: `./skip_list.txt`)

## How failures get triaged

Each FAIL falls into one of three buckets:

1. **CONFIG gap** — add the missing `CONFIG_*` define to `linux_fix.h`.
2. **Arch-specific / non-arm64 file** — the real kernel doesn't build it
   on arm64 either; add to `skip_list.txt`.
3. **Real ncc compiler bug** — fix in `src/`, add a minimal repro to
   `tests/regression/`, commit per `docs/main-commit-contract.md`.

Bug fixes flushed out by past Linux scans are visible in the git log
under `parse:`, `codegen_arm64:`, `tokenize:`, `preprocess:` subjects
(e.g. `7abd79f`, `6d55ec3`, `06cbee9`, `c760de4`, `3ca7a5f`, `ad2db17`).

## Subsystems known to be at 100% pass

(As of last sweep, commit `d144a05` on 2026-04-29.)

- `drivers/pci`, `drivers/ata`
- `fs/btrfs`, `fs/fuse` (with FUSE_DAX + VIRTIO_FS configs)
- `drivers/{hid,thermal,usb/host,scsi}` and `sound/usb` (Phase B sweep)

A full sweep across all subsystems would be a good "where do we stand"
snapshot when the workstream resumes.

## License

Scan infra and stubs in this directory are MIT (the ncc license). The
Linux kernel sources referenced by the scan are GPL-2 (Linux's own
license).
