# AArch64 C compiler compliance kit

A self-contained kit for checking whether a C compiler conforms to
the contract in `SPEC.md`. Drop this directory anywhere, point `CC`
at the compiler under test, and run `./check.sh`.

## Layout

```
.
├── SPEC.md            Implementation contract (the spec being tested)
├── check.sh           Top-level runner (runtime suite + integration checks)
├── tests/
│   ├── run.sh         Runtime test runner
│   ├── README.md      Test-by-test description
│   └── NN_*.c         18 self-checking C programs
└── README.md          This file
```

## Quick start

```sh
# Run everything with the default 'cc'.
./check.sh

# Point at a specific compiler.
CC=/path/to/your-compiler ./check.sh

# With optimization (recommended; see "Optimization note" below).
CC=/path/to/your-compiler CFLAGS="-O2" ./check.sh
```

Exit status is 0 if all stages pass, 1 otherwise.

## What it checks

`check.sh` runs three stages:

1. **Runtime test suite** (`tests/run.sh`) — 18 self-checking C
   programs. Each one returns 0 on pass, non-zero on fail. Per-test
   spec references are in `tests/README.md`.
2. **Section-attribute integration** — verifies that
   `__attribute__((section("name")))` actually places a symbol in
   the named section in the produced object file, using `readelf -S`
   or `objdump -h`.
3. **`-x assembler-with-cpp` / `__ASSEMBLER__`** — verifies that
   driving the compiler to preprocess assembly predefines
   `__ASSEMBLER__`.

Stages 2 and 3 are best-effort: they skip cleanly if the relevant
tool isn't available (no `readelf`, no driver support for `-x`).

## What it doesn't check

Some contracts in `SPEC.md` need manual verification or
build-system-level integration:

- **§2.5 `-no-fp-varargs`** — kernel-style flag to suppress SIMD
  register saves in the variadic prologue. Verify by inspecting the
  generated assembly.
- **§2.7 `long double` literals on Linux** — Linux's `strtold`
  returns extended precision; verify the compiler accepts and
  truncates without crash.
- **§4.7 link via `cc` driver on Linux** — implicitly exercised by
  any test in this suite that compiles and links.
- **§4.8 `<limits.h>` `#include_next` chain** — implicitly exercised
  on Linux by any test that pulls in libc headers.
- **Self-bootstrap fixed point** — build the compiler with itself
  (stage1), then again with stage1 (stage2), verify
  `md5(stage1) == md5(stage2)`. Project-specific build glue, so
  not in this kit.

## Optimization note

Test `15_static_inline_asm_n` requires the implementation to inline
the `static inline` body regardless of optimization level (SPEC §4.4).
A compiler that relies on the optimizer to inline `static inline`
functions will only pass this test at `-O` and above.

## Interpreting failures

Each test returns a small, distinct non-zero exit code per check it
makes. When a test fails the runner prints the exit code; cross-
reference it with the source to localize the bug. For example:

```
FAIL  07_va_named_overflow  (exit=2; see /tmp/aarch64spec_07_va_named_overflow.run.log)
```

means `test()` returned 2, which from the source is "the variadic
sum was wrong" — pointing at `__stack` calculation rather than at
the named-param values.

Compile-time failures print the path of the compiler's stderr log so
you can read the diagnostic directly.

## License

Public domain / CC0. Use freely.
