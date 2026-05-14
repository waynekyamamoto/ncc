# AArch64 C compiler — implementation contract

Target spec for a C compiler that can build and self-bootstrap a
NetBSD-10 evbarm64 kernel and self-host on Linux/glibc.

Each contract below is stated as a requirement, paired with a
small **falsifiable test** in `tests/`. Implementation is free.

---

## 1. Apple ARM64 / Mach-O vs. AAPCS64 / ELF

The implementation must support two distinct targets:

- **Apple ARM64 / Mach-O** (default on Apple hosts)
- **AAPCS64 / ELF** (default on non-Apple hosts; explicitly selectable)

The targets differ in:

| Area | Apple | AAPCS64 / ELF |
|---|---|---|
| Variadic args 1..8 | always on stack | in x0-x7 (GP) / d0-d7 (FP) |
| `va_list` | `char *` | 32-byte struct (see §2) |
| Section names | `__TEXT,__text` etc. | `.text` / `.data` / `.bss` / `.rodata` |
| Symbol leading underscore | yes | no |
| Symbol relocation syntax | `@PAGE` / `@PAGEOFF` | `:lo12:` |
| Weak symbol | `.weak_definition` | `.weak` |
| `.subsections_via_symbols` | emitted | not emitted |
| Predefines | `__APPLE__`, `__MACH__`, … | `__ELF__`, plus `__linux__` etc. on Linux |
| Linker invocation | `ld` + `-syslibroot` + `-lSystem` | `cc` as driver + `-no-pie` |
| Assembler | host `as` | `aarch64-elf-as` or `aarch64--netbsd-as` |

The ELF target must **not** predefine the macOS-only macros
`__APPLE__`, `__MACH__`, `__DARWIN_C_LEVEL`, `TARGET_OS_MAC`,
`TARGET_OS_OSX`, `TARGET_RT_MAC_MACHO`.

When targeting ELF, predefine `__ARM_ARCH=8`, `__ARM_ARCH_8A__`,
`__ARM_PCS_AAPCS64`.

On Linux, the bundled-include directory must be discoverable from the
running compiler binary (standard idiom: `/proc/self/exe`).

Pass `-march=armv8.6-a+sve` to the cross-assembler in ELF mode.

Accept (without warning) `-nostdinc` and `--sysroot=`.

---

## 2. AAPCS64 variadic ABI

The reference is AAPCS64 §7.1.5 and the GCC `<stdarg.h>` ABI.

### 2.1 `va_list` layout

`va_list` is a 32-byte struct:

```
struct {
    void *__stack;      // overflow stack area
    void *__gr_top;     // top of GP register save area
    void *__vr_top;     // top of FP/SIMD register save area
    int   __gr_offs;    // offset from __gr_top to next GP slot (negative)
    int   __vr_offs;    // offset from __vr_top to next FP slot (negative)
};
```

`va_list` passed by value follows the AAPCS64 rule for structs >16
bytes: it is passed via memory.

`__builtin_va_list` and `__gnuc_va_list` must both alias this struct
on ELF.

### 2.2 Variadic prologue

A function with variadic args under AAPCS64 must, at entry:

1. Reserve a GP register save area of 64 bytes (8 × 8). Store `x0..x7`
   into it, accounting for how many GP regs were consumed by the
   **named** parameters.
2. Reserve a VR register save area of 128 bytes (8 × 16). Store
   `q0..q7` (16-byte stride) into it, accounting for FP/SIMD regs
   consumed by named params.
3. `va_start` sets:
   - `__stack`  → first stack-passed variadic argument
   - `__gr_top` → top of GP save area (just past x7's slot)
   - `__vr_top` → top of VR save area (just past q7's slot)
   - `__gr_offs` = -(8 - n_named_gp) * 8
   - `__vr_offs` = -(8 - n_named_fp) * 16

### 2.3 `__stack` when named args overflow

When a function has more than 8 named GP params, named args 9..N
spill onto the stack at `x29+16`, `x29+24`, … Variadic args start
**after** those slots.

`__stack` base offset = `16 + (gp_overflow + fp_overflow) * 8`
where `*_overflow = max(0, n_named_* - 8)`.

### 2.4 `va_arg` FP path

Float/double variadic args read from the VR save area when
`__vr_offs < 0`. `__vr_offs` advances by 16 (not 8) per arg.
The stack overflow area is used when `__vr_offs >= 0`. The GP path
is symmetric with 8-byte stride.

### 2.5 FP-suppressed variadic mode

For freestanding/kernel builds compiled with `+nofp+nosimd`, the
implementation must provide a way to:

- Suppress VR register saves in the variadic prologue.
- Zero `__vr_top` and `__vr_offs` so `va_arg` always takes the stack
  path for FP args.

### 2.6 Apple vs ELF caller-side discipline

On Apple ARM64, the caller pushes all variadic args on the stack.
On ELF / AAPCS64, the caller must follow the AAPCS64 register/stack
split (first 8 GP variadic args in x0-x7, etc.). Stack-only on ELF
is not ABI-compatible with glibc.

### 2.7 `long double` literals on Linux

Use a 64-bit-double-precision floating-constant parser for source
literals. Linux `strtold` returns extended precision that does not
fit a 64-bit float representation.

---

## 3. Correctness contracts

### 3.1 64-bit case constants

Case constants, alignment-attribute values, vector-size-attribute
values, and array-range-init bounds must be evaluated and
represented as 64-bit signed integers end-to-end, without
intermediate narrowing to `int`.

Test: `tests/01_switch_bit31.c`.

### 3.2 FP-typed constant folding

Constant folding must not evaluate a node of type `float`, `double`,
or `long double` as an integer.

Test: `tests/02_fp_const_fold.c`.

### 3.3 `mov xN, xN` vs `mov wN, wN`

`mov xN, xN` is a no-op and may be elided. `mov wN, wN` zero-extends
the low 32 bits into the upper 32 and must not be elided.

Test: `tests/03_w_mov_widening.c`.

### 3.4 Forward static-function references in file-scope initializers

Inside a file-scope initializer, identifier lookup must allow
forward references to functions defined later in the same
translation unit.

Test: `tests/04_fwd_static_init.c`.

---

## 4. C source-compatibility features

### 4.1 `__gnuc_va_list` predefined typedef

`__gnuc_va_list` must be available as a typedef equivalent to
`__builtin_va_list`. On ELF it is the AAPCS64 struct (§2.1); on
Apple it is `char *`.

Test: `tests/17_gnuc_va_list.c`.

### 4.2 `__sync_*` GCC builtins (inline emission)

The implementation must lower `__sync_*` builtins inline (no libgcc
dependency):

| Builtin | Emission |
|---|---|
| `__sync_synchronize()` | `dmb ish` |
| `__sync_lock_release(p)` | `stlr wzr, [p]` (size-dependent) |
| `__sync_lock_test_and_set(p, v)` | `ldaxr` / `stlxr` retry loop |
| `__sync_bool_compare_and_swap(p, old, new)` | `ldaxr` / `cmp` / `stlxr` CAS |

Test: `tests/18_sync_builtins.c`.

### 4.3 No-op attribute parsing

Parse-and-ignore (without warning) the function attributes
`target`, `pcs`, `no_sanitize` and their argument forms.

Test: `tests/14_noop_attrs.c`.

### 4.4 Asm-bodied static-inline functions with constant constraints

The implementation must inline `static inline void` functions whose
body is a single `__asm__` statement, at every call site,
regardless of optimization level. The asm template must see
compile-time-constant call arguments as immediates (so the `"n"`
input constraint is satisfied).

Test: `tests/15_static_inline_asm_n.c`.

### 4.5 `__attribute__((section("name")))` emission

The implementation must emit symbols with `section("name")` into a
section named exactly `name`. Under ELF, emit explicit flags
`"a", %progbits` in the section directive.

### 4.6 Driver: `-x` and `__ASSEMBLER__`

- Consume `-x <lang>` and its argument as a flag pair.
- When preprocessing `.S` files or `-x assembler-with-cpp` input,
  predefine `__ASSEMBLER__=1`.

### 4.7 Link via `cc` driver on Linux

On Linux, drive the link via `cc`, not by invoking `ld` directly.
Pass `-no-pie`.

### 4.8 `<limits.h>` and `#include_next`

A bundled `<limits.h>` that chains via `#include_next` must define
`_GCC_LIMITS_H_` before the chain, to prevent infinite recursion
with glibc's `<limits.h>`.
