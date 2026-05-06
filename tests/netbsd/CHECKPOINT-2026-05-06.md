# NetBSD/aarch64 checkpoint — 2026-05-06

Tag: `netbsd-login`. Working branch: `main`.

## What works

`tests/netbsd/build.sh MINIMAL_VIRT64 boot` produces an ncc-built kernel
that boots under QEMU virt to a **login prompt** against a GPT rootfs.
Every `.c` in the kernel build goes through ncc — the wrapper has zero
`kern/*.c` gcc routes.

Reproduce:

```bash
cd tests/netbsd
./build.sh MINIMAL_VIRT64 boot
```

(Assumes `~/netbsd/src` is a `netbsd-10` checkout with `tools` already
built. See README.md for one-time setup.)

## What's stubbed (intentional, not ncc bugs)

- **NEON crypto** (`chacha_neon.c`, `aes_neon.c`) — ncc has no NEON
  intrinsics. `chacha: self-test failed` at boot is expected; no
  functional impact under QEMU virt without NEON.
- **SoC cfattachs** (~67) — Tegra, Rockchip, Apple, Sunxi, etc. Not
  relevant to QEMU virt.
- `syscalls_autoload.c` — ncc OOMs on the 600-entry table. Empty stub.

## Compiler features that landed for this push

Commits accumulated since `2d44324` (NetBSD assets brought in
2026-05-01) through 2026-05-04. The load-bearing ones for boot →
login:

| Commit | What |
|---|---|
| `e7e7393` | `va_start` correct when >8 named GP/FP params (sysctl_createv was the proxy) |
| `ff529fb` | Forward static fn references in file-scope initializers |
| `93c6ecc` | Section attributes, file-scope `__asm`, mini-inliner, ARM_ARCH predefines, compound-literal-postfix, `__asm` keyword |
| `4ed0320` | `-target elf` enhancements, ARM arch predefines, Linux build portability |
| `8fe8dda` | `-target elf` flag + `__sync_*` builtins |
| `9e6b44c` | Case constants are `int64_t`, not `int` (fixed tty ioctl miscompile) |
| `6fe6f4e` | `const_expr_val` returns `int64_t` everywhere |
| `150f17d` | `try_eval_node` doesn't evaluate FP-typed nodes as int64 |
| `d5f7431` | `__gnuc_va_list` recognized as typedef name |

Plus peephole pass (`270f3dc`) — not load-bearing for boot, but a
~9% `.text` reduction that this work motivated.

## Open ncc bugs surfaced by NetBSD (not blocking login)

See `STATUS.md` for the deep history; summary of what's still real:

- **Forward typedef visibility** in 4 kern/ files (`register_t`,
  `kgdb_reg_t`, `physmap_t`, `Bytef`) — currently routed around.
- **Parser-state corruption mid-file** in 3 sanitizer files
  (`subr_asan.c`, `subr_fault.c`, `subr_kcov.c`) — not yet reproduced.
- **OOM** on `syscalls_autoload.c` — file is stubbed.
- **NEON intrinsics** — months of compiler work; stubbed.
- **Variadic with ≤8 named GP/FP params + variadic** still needs a
  register save area; the current fix only handles the spill case
  (the kernel doesn't exercise the gap).

## What this checkpoint does NOT cover

- Past `root device:` → login is reached, but interactive userland
  hasn't been driven. Boot-test verifies banner + login prompt.
- GENERIC64 still doesn't boot (this is MINIMAL_VIRT64 only). Size
  bloat and several SoC-driver stubs remain in the way; not on the
  current path.
- Linux kernel scan: suspended (tests/linux/).
- xv6: suspended (tests/xv6/).

## Pointers

- `STATUS.md` — full session-by-session history through 2026-04-30.
- `reference-boot.log` — the original Phase 1 (root device:) capture
  from 2026-04-30. Stale relative to current state; kept as anchor.
- `tools/ncc-elf-wrapper.sh` — the live build wrapper. Inspect to see
  what's still routed away from ncc (NEON/SoC stubs, no kern/*.c).
