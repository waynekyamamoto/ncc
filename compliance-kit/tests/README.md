# AArch64 C compiler — compliance test suite

Falsifiable runtime tests for the contracts in `../SPEC.md`. Each
test is a self-checking C program: returns 0 on pass, a small
non-zero exit code on fail. No golden-output comparison.

## Usage

```sh
./run.sh                           # uses $CC, defaults to 'cc'
CC=gcc ./run.sh
CC=clang CFLAGS="-O2" ./run.sh
CC=/path/to/your-compiler ./run.sh
```

Exit 0 if every test passes, 1 if any failed. A test that fails to
compile counts as a failure. Per-test logs land in `$TMPDIR`
(default `/tmp`) and are kept only for failures.

## What's covered

| Test | Spec ref | What it checks |
|---|---|---|
| 01_switch_bit31 | §3.1 | case constants with bit 31 set match unsigned switch |
| 02_fp_const_fold | §3.2 | constant folder respects FP operand types |
| 03_w_mov_widening | §3.3 | `mov wN,wN` zero-extension preserved |
| 04_fwd_static_init | §3.4 | forward static-fn refs in file-scope initializers |
| 05_va_basic | §2 | variadic baseline |
| 06_va_all_regs | §2.2 | variadic GP register save area + stack overflow |
| 07_va_named_overflow | §2.3 | `__stack` offset when named GP params spill |
| 08_va_double | §2.4 | variadic FP path, VR save area, 16-byte stride |
| 09_va_copy | §2 | `va_copy` produces an independent walker |
| 10_designated_init_or | C-std | designated init with bitwise-OR const-expr RHS |
| 11_named_no_varargs | §2 | 12 named int params stack-passing baseline |
| 12_small_struct_return | §2 | small struct returned in x0/x1 |
| 13_asm_rr | §4 | inline asm "=r"/"r" round-trip |
| 14_noop_attrs | §4.3 | parse-and-ignore `target`, `pcs`, `no_sanitize` |
| 15_static_inline_asm_n | §4.4 | asm-bodied static-inline w/ `"n"` constraint |
| 16_adrp_lo12 | §1 | file-scope global address via function pointer |
| 17_gnuc_va_list | §4.1 | `__gnuc_va_list` typedef visible |
| 18_sync_builtins | §4.2 | `__sync_*` builtins lowered inline |

## Contracts not covered by a runtime test

Some contracts need driver-level, link-level, or object-inspection
checks. The top-level `../check.sh` runs the section-attribute (§4.5)
and `__ASSEMBLER__` (§4.6) checks. The remainder are manual:

- **§2.5 FP-suppressed variadic mode** — pass the implementation's
  flag for this mode and inspect the variadic prologue for absence
  of `stur d0..d7`.
- **§2.7 `long double` literals on Linux** — feed an extended-
  precision literal; verify the compiler accepts it.
- **§4.7 link via `cc` on Linux** — implicitly exercised by every
  test in this suite that compiles and links on Linux.
- **§4.8 `<limits.h>` `#include_next` chain** — implicitly exercised
  on Linux by any test that pulls in libc headers.

## Notes

- Tests assume an AArch64 host; `13_asm_rr` and
  `15_static_inline_asm_n` use AArch64-specific assembly.
- A failing test exits with a small distinct code per check; cross-
  reference with the source to localize the failure.
- Tests don't include `<stdio.h>`; failures are silent at runtime.
  To debug, edit the test to print intermediate values or run it
  under a debugger.
- Test 15 requires call-site inlining of `static inline void`
  functions regardless of optimization level (SPEC §4.4). A
  compiler that relies on the optimizer to inline `static inline`
  bodies will only pass at `-O` and above.
