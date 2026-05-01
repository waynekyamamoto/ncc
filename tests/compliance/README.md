# compliance — language conformance tests for ncc

These tests are the contract ncc must satisfy.  Each one is a complete
C program that:

- compiles with `clang` (host) and produces some deterministic output
- compiles with `ncc` and must produce **byte-identical** output

The harness is `run.sh`.  Run from this directory; it writes binaries
to `/tmp/compliance_{clang,ncc}_<name>` and reports PASS/FAIL/SKIP.

## What each test exists for

The first 15 tests (01–15) are inherited from the chibicc test suite
and cover language basics: type sizes, calling convention, structs,
bitfields, casts, integer promotions, etc.

The remaining tests (16–31) lock in patterns that came up during the
NetBSD/aarch64 boot bring-up.  Each is a falsifiable regression test
tied to a specific bug fix or extension:

| Test | Covers | Tied to |
|---|---|---|
| 16 | static-inline `__asm` mini-inliner | NetBSD PSTATE writes (`msr daifset, %0 :: "n"(val)`); SKIPs at clang side |
| 17 | `__attribute__((section("foo")))` capture and emit | Link-set markers, `__read_mostly`, `__cacheline_aligned` |
| 18 | File-scope `__asm("...")` directives | NetBSD `__strong_alias` / `__weak_alias` macros |
| 19 | Compound literal followed by `[]`/`.`/`->` postfixes | ACPICA `(union {...}){ x }._t` pattern |
| 20 | `if (constant)` folding via `try_eval_node` | NetBSD `daif_disable()`'s `if (!__builtin_constant_p(p))` |
| 21 | Forward-static-fn in file-scope initializer | NetBSD `kern_cctr.c` timecounter init |
| 22 | Variadic `va_start` with > 8 named GP/FP params | **Boot blocker** — NetBSD `sysctl_createv` (12 named ints) before commit `579f490` |
| 23 | Variadic with all variadic args fitting in registers | Confirms ncc's stack-only convention is internally consistent |
| 24 | File-scope struct designated init with OR'd constants | Diagnostic dead-end during sysctl debug; locks in correct behaviour |
| 25 | `va_copy` walking the same list twice | Stdarg primitive |
| 26 | `&global` ADRP+ADD:lo12 for high-VA globals + call through ptr | Diagnostic dead-end during sysctl debug; locks in correct behaviour |
| 27 | Inline asm `__asm__("..." : "=r"(r) : "r"(x))` round-trip | Kernel asm primitive |
| 28 | No-op attributes accepted (`target`, `pcs`, `no_sanitize`, …) | Pervasive across kernel sources |
| 29 | Variadic with `double` args | FP overflow path of va_area calc |
| 30 | Function with 12 named int params, no variadic | AAPCS64 stack-arg passing baseline |
| 31 | Function returning a struct (≤ 16 bytes, via x0/x1) | AAPCS64 struct-return calling convention |

## When to add a test here

- A specific kernel pattern broke and you fixed it — add a test that reproduces the bug pre-fix and passes post-fix.
- A diagnostic hypothesis you want to lock in (whether the hypothesis was right or wrong).
- A language feature added to ncc's parser or codegen.

Avoid tests for chibicc-shaped internals (e.g. testing parser intermediate states).  Test **observable behaviour** only — what gets compiled and run.

## Beyond this suite

Three larger tests exist outside this directory; together they form the
full ncc validation pyramid:

1. **Bootstrap fixed point** — `bootstrap_validate` from `CLAUDE.md`.
   ncc compiling itself must produce a binary that compiles itself
   byte-identically.  Strongest correctness signal.

2. **DOOM build** — `build_doom_ncc2.sh` from the project root.
   83 C files compiled by `ncc2`, linked with clang+AppKit, runs.
   Stress test for the Mach-O code path.

3. **NetBSD/aarch64 kernel build + boot** — see
   [waynekyamamoto/netbsd-port](https://github.com/waynekyamamoto/netbsd-port).
   Full GENERIC64-style MINIMAL_VIRT64 kernel built end-to-end with
   `tools/docker-kernel-build.sh`, must reach `root device:` under
   QEMU virt.  Stress test for the ELF code path and AAPCS64 ABI.

A new ncc passes the rewrite bar iff it passes (1), (2), (3), AND
every test in this directory.
