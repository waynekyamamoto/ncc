# ncc Preprocessor — English Specification (Phase 2)

This is the source of truth for ncc's preprocessor. The implementation
in `src/preprocess.c` should be re-derivable from this document without
referring to chibicc or the pre-Phase-2 source. When the implementation
and this document disagree, the document is authoritative; bugs in the
implementation are fixed against the document, the document is updated
only when the behavior is intentionally changed.

**Status (2026-05-03):** This document is a skeleton. Sections §1, §13,
§14, and §15 are drafted. Sections §2–§12 (the algorithmic core) are
section headers + open design questions, awaiting answers in
`docs/specs/02_preprocessor_questions.md` (Q1–Q21). The skeleton
defines the shape of the eventual document and the dependency between
each algorithmic section and the design decisions it relies on.

**Phase 2 scope:** macOS / AArch64 only. The `-target elf` predefines
from `main` commits `4ed0320` and `8fe8dda` (`__ELF__`, `__ARM_ARCH`,
`__ARM_ARCH_8A__`, `__ARM_PCS_AAPCS64`) are explicitly **out of scope**
and are deferred to Phase 5; see §15.

---

## 1. Scope

The preprocessor takes a tokenized translation unit (a `Token *` list
produced by `tokenize` + `tokenize_file`) and produces a transformed
`Token *` list with all preprocessor directives executed and all macro
invocations expanded. Its output is the input to the parser.

**Inputs:**
- A `Token *` list, terminated by `TK_EOF`. Tokens carry the fields
  established by the tokenizer (`kind`, `loc`, `len`, `at_bol`,
  `has_space`, etc.; see `01_tokenizer.md` §3).
- Module state: include-path list, macro table, `#pragma` callback,
  conditional-inclusion stack, monotonic counter for `__COUNTER__`,
  base-file pointer for `__BASE_FILE__`. (See §3 for how this state
  is named / scoped.)

**Outputs:**
- A `Token *` list, terminated by `TK_EOF`, with:
  - All directive lines (`#define`, `#if`, `#include`, etc.) consumed
    and removed from the stream.
  - All macro invocations expanded, with hideset rules applied.
  - All `__FILE__` / `__LINE__` / `__COUNTER__` / `__TIMESTAMP__` /
    `__BASE_FILE__` references replaced with their values.
  - All `TK_PP_NUM` tokens converted to `TK_NUM` via
    `convert_pp_tokens` (the same conversion the tokenizer's
    post-pass would do, but deferred until after macro expansion so
    that pasting can build new pp-numbers).

**Interface to the rest of the compiler:**
- `preprocess(Token *tok)` — top-level entry. Called once per
  translation unit by the driver after `tokenize_file` returns the
  raw token stream.
- `init_macros(void)` — called once at compiler startup to register
  predefined macros and handler macros.
- `define_macro(char *name, char *body_text)` /
  `undef_macro(char *name)` — called from the driver for `-D` / `-U`
  command-line flags.
- `add_include_path(char *path)` — called from the driver for each
  `-I` flag.
- `set_pragma_handler(PragmaHandler *fn)` — installs a callback that
  receives `#pragma` lines the preprocessor doesn't handle internally.

**Two passes that the preprocessor is *not* responsible for:**
- The tokenizer (per `01_tokenizer.md`) does the lexing, including
  setting `at_bol` and `has_space` precisely. The preprocessor relies
  on these flags but never sets them on freshly-emitted tokens (only
  via `copy_token` inheritance and re-tokenization inside `stringize`
  and `paste`).
- The parser does syntactic analysis. The preprocessor produces a
  fully-expanded token stream; the parser sees no `#`-prefixed
  directives and no unexpanded macros.

---

## 2. Data model

> **Awaiting:** Q14 (POSIX-replacement strategy — affects which
> internal helpers are part of the data model), Q16 (module state
> shape — globals vs context struct).

This section will describe the persistent data structures the
preprocessor operates on:

- **`Macro`**: name, body token list, parameter list (function-like),
  variadic-arg-name (function-like), `is_objlike` flag, `handler`
  callback (for `__FILE__` etc.), `deleted` flag.
- **`MacroParam`**: linked list of parameter names.
- **`MacroArg`**: one per actual argument at expansion time; raw
  tokens (`tok`), pre-expanded tokens (`expanded`), and parameter
  name.
- **`Hideset`**: linked list of `char *name`. (Spec describes the
  semantic operations; per Q3, dedup is permitted as an optimization
  but not required.)
- **`CondIncl`**: linked-stack node tracking conditional-inclusion
  state — `tok` (the directive), `ctx` (`IN_THEN`/`IN_ELIF`/`IN_ELSE`),
  `included` (whether any branch of this `#if` chain has been
  selected yet).
- **Macro table**: `HashMap` keyed by macro name (string).
- **Include-path list**: `StringArray` of search directories.

### 2.1 Token field consumption

The preprocessor reads almost every field of `Token`. For each, the
spec will state whether the preprocessor relies on the tokenizer's
initial value, mutates it, or both. (See `01_tokenizer.md` §3 for the
tokenizer's definitions.)

Critical reliances:
- `tokenizer-set hideset` must be `NULL` for hideset operations to
  work correctly.
- `tokenizer-set origin` must be `NULL` for the origin chain to
  terminate cleanly.
- `tokenizer-set at_bol` is load-bearing for `is_hash` (directive
  detection); preprocessor never modifies it directly.
- `tokenizer-set has_space` is load-bearing for the function-like
  macro detection rule (see Q7).

---

## 3. Module state

> **Awaiting:** Q16 (globals vs context struct).

The preprocessor maintains state across calls within one translation
unit: macro table, include paths, conditional-inclusion stack,
counter, pragma handler, base-file pointer. This section will describe
the logical interface (init / register / preprocess / teardown) and
leave the physical representation (globals vs context) to the
implementation per Q16's recommendation.

---

## 4. Top-level entry: `preprocess(Token *tok)`

> **Awaiting:** none — this section is a thin wrapper around §5; will
> be drafted alongside the skeleton.

The top-level entry point:
1. Resets `pp_token_count`.
2. Calls the main loop `preprocess2(tok)`.
3. Errors if the conditional-inclusion stack is non-empty (unterminated
   `#if`).
4. Calls `convert_pp_tokens` on the result to lower `TK_PP_NUM` to
   typed `TK_NUM`.
5. Returns the resulting token stream.

---

## 5. Main directive-dispatch loop: `preprocess2(Token *tok)`

> **Awaiting:** none algorithmic; depends on §6–§12 for the per-directive
> behavior.

The main loop walks the token stream, dispatching on `at_bol && '#'`
to per-directive handlers and macro-expanding non-directive tokens via
`expand_macro`. Recursively re-entered by `eval_const_expr` (to expand
macros inside `#if` expressions) and by `read_include_filename` (to
expand a macro that produces a filename).

Per-directive dispatch table:
- `#include`, `#include_next` → §10
- `#define`, `#undef` → §6 (macro definition)
- `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif` → §8
- `#line` → §12
- `#pragma`, `#error`, `#warning` → §12
- (null directive — `#` followed by newline) → no-op
- (unknown directive) → error

---

## 6. Macro expansion

> **Awaiting:** Q3 (hideset dedup), Q8 (rescan/stringize asymmetry —
> requires worked example), Q9 (painter's rule — requires worked
> example), Q10 (origin propagation rule), Q12 (line_delta on expanded
> tokens).

### 6.1 Macro definition (`#define` / `#undef`)
> Function-like detection rule per Q7 (cite C11 wording).

### 6.2 `expand_macro` — single-step expansion
> Hideset check → `find_macro` → dispatch (handler / object-like /
> function-like). Object-like uses union(tok->hideset, {m->name});
> function-like uses union(intersection(macro_tok->hideset,
> rparen->hideset), {m->name}) per Q9.

### 6.3 `read_macro_args` — argument collection
> Including `handle_pp_directive_in_arg` for the Linux-style pattern.

### 6.4 `subst` — argument substitution into body
> Including `__VA_OPT__`, GNU `, ## __VA_ARGS__`, the rescan rule
> (Q8: substitution uses `arg->expanded`, stringize uses `arg->tok`).

### 6.5 Builtin handler macros
> `__FILE__`, `__LINE__`, `__COUNTER__`, `__TIMESTAMP__`,
> `__BASE_FILE__`. `__TIMESTAMP__` returns "Unknown" (Q6).

---

## 7. Stringize (`#`) and paste (`##`)

> **Awaiting:** Q8 (rescan asymmetry detail).

### 7.1 `#` — stringize
> Re-tokenize via `tokenize` over a buffer built by joining `arg->tok`
> spellings (raw, not pre-expanded) with `has_space` controlling
> spacing. Leading-token `has_space` ignored (see inventory note).

### 7.2 `##` — paste
> Concatenate `lhs->loc[0..len)` and `rhs->loc[0..len)`, re-tokenize,
> error if more than one token results.

### 7.3 Empty-arg paste
> When LHS or RHS of `##` is an empty-expansion arg, the paste is
> skipped in `subst` (not in `paste`). Spec must call this out.

### 7.4 Chained paste
> `A##B##C##D` semantics from `subst`'s inner loop.

---

## 8. Conditional inclusion

### 8.1 `#if`, `#ifdef`, `#ifndef`
> `#if` evaluates the constant expression via §9. `#ifdef`/`#ifndef`
> use direct hashmap lookup (no macro expansion of the operand).

### 8.2 `#elif`, `#else`
> State machine: error on `#elif` or `#else` after `#else`; track
> `included` to skip subsequent branches once one is taken.

### 8.3 `#endif`
> Pop the `cond_incl` stack.

### 8.4 `skip_cond_incl`
> Walks the token stream skipping a not-taken branch; recursively
> skips nested `#if*` blocks via `skip_cond_incl2`.

### 8.5 `handle_pp_directive_in_arg`
> Conditional directives encountered inside a macro argument list
> share the same `cond_incl` stack as the main loop.

---

## 9. Constant-expression evaluation (`eval_const_expr`)

> **Awaiting:** none specific.

Pipeline (per inventory §2.2):
1. `read_const_expr`: copy the line, inline-substitute `__has_*`
   intrinsics and `defined(X)`.
2. Recursive `preprocess2` to expand macros in the expression.
3. Post-expansion second pass for `defined(X)` that surfaced from
   macro expansion.
4. Replace remaining identifiers with `0`.
5. `convert_pp_tokens`.
6. Recursive-descent evaluator (`const_expr` → `cond` → `logor` …
   → `primary`).

`__has_attribute` and `__has_builtin` consult an allowlist (Q13).
`__has_feature` returns 0 (Q5). `__has_include` /
`__has_include_next` check file existence via the include-path
search.

---

## 10. `#include` resolution

### 10.1 `#include "..."` and `#include <...>`
> `"..."` searches dirname of current file first, then
> `include_paths`. `<...>` only `include_paths`.

### 10.2 `#include_next`
> Find current file's directory in `include_paths`, search from the
> following index.

### 10.3 `read_include_filename`
> Three patterns: `TK_STR`, `<...>` (concatenated tokens), `TK_IDENT`
> (recursive macro expansion).

### 10.4 `search_include_paths`
> Linear scan, file-existence probe via `access(R_OK)` per Q2 (was
> `fopen` in main; new spec uses `access` to fix the FD leak).

### 10.5 `include_file` — splicing
> Per Q11: spec describes the splice as logically equivalent to
> appending the included token stream before the continuing token;
> implementation may mutate the included file's terminal `TK_EOF`
> sentinel for efficiency.

---

## 11. Predefined macros (`init_macros`)

> **Awaiting:** Q14 (POSIX-replacement strategy may affect helper
> structure), Q15 (`__STDC_NO_ATOMICS__` vs `__sync_*` contradiction
> resolution), Q18 (target dispatch for ELF predefines later).

The macOS predefine set, organized by category, with each macro
listed exactly once (Q1: deduplicate `__SCHAR_MAX__`):

- **Standard C** (`__STDC__`, `__STDC_VERSION__`=201112L,
  `__STDC_HOSTED__`, etc.).
- **Platform sizes/types** (`__LP64__`, `__SIZEOF_*`, `__SIZE_TYPE__`,
  etc.).
- **Limits** (`__SCHAR_MAX__`, `__SHRT_MAX__`, etc.). De-duplicated
  per Q1.
- **Float constants** (`__FLT_*`, `__DBL_*`, `__LDBL_*`).
- **Byte order** (`__ORDER_LITTLE_ENDIAN__`, `__BYTE_ORDER__`, etc.).
- **ARM64/AArch64** (`__aarch64__`, `__arm64__`, `__arm64`,
  `__AARCH64EL__`).
- **Apple platform** (`__APPLE__`, `__MACH__`, `__DARWIN_C_LEVEL`).
- **Apple target conditionals** (`TARGET_OS_*`, `TARGET_CPU_*`,
  `TARGET_RT_*`).
- **Darwin deployment target** (`__MAC_OS_X_VERSION_*`).
- **GCC compatibility** (`__GNUC__`=12, etc., keyword and type
  aliases).
- **GCC atomic order constants** (`__ATOMIC_*`).
- **GCC atomic builtin stubs** (`__atomic_*`,
  `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_*`). Per Q15, these coexist with
  `__STDC_NO_ATOMICS__`=1 deliberately.
- **Builtin function-like macros** (`__builtin_expect`, fabs,
  inf/nan, `__builtin_offsetof`, `__builtin_unreachable`, etc.).
- **Date/time** (`__DATE__`, `__TIME__` fixed at `init_macros` call
  time).
- **Handler macros** registered with `add_macro` + `m->handler`:
  `__FILE__`, `__LINE__`, `__COUNTER__`, `__TIMESTAMP__`,
  `__BASE_FILE__`.

The full list with values is reproduced as a table in this section
when it's drafted.

---

## 12. `#line`, `#pragma`, `#error`, `#warning`

### 12.1 `#line`
> Sets `current_file->line_delta` to `directive_value -
> tok->line_no`. Optional second token is a string literal that
> sets `display_name`.

### 12.2 `#pragma`
> `#pragma once` is a stub (Q4: keep stub for Phase 2; add regression
> test). All other pragmas pass to the registered `pragma_handler`
> callback.

### 12.3 `#error` / `#warning`
> Concatenate the rest-of-line tokens and call `error_tok` /
> `warn_tok`.

---

## 13. Known divergences from ISO C / GCC

These are intentional simplifications or quirks preserved for
behavioral compatibility with the pre-Phase-2 implementation
(matched against `main`'s preprocessor today). Recorded here so a
re-implementer does not "fix" them by accident.

- **`#pragma once` is a no-op stub** (Q4). Re-`#include`ing a file
  with `#pragma once` will include it again. Most modern headers use
  include guards, so the divergence rarely surfaces; some Apple SDK
  headers and third-party libraries may behave differently than
  under clang.

- **`__has_feature(X)` always returns 0** (Q5) for every X. Headers
  that gate features via clang's `__has_feature` will take the
  fallback path. For C11 code targeting macOS this is almost always
  the right path; for C++ code it would not be (but ncc does not
  compile C++).

- **`__TIMESTAMP__` returns the literal string `"Unknown"`** (Q6).
  Source-file mtime is not stat'd. Banner-style uses are visibly
  affected; correctness-style uses (rare) take a constant value.

- **`__DATE__` / `__TIME__` fixed at `init_macros()` call time**, not
  per-translation-unit. If the driver compiles multiple TUs in a
  single invocation, all share the same date/time. Currently a
  non-issue since each TU is a separate ncc process.

- **`__STDC_NO_ATOMICS__` = 1 and `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_*`
  = 1 are both advertised** (Q15). Standards-compliant headers see
  no atomics support; GCC-compat headers see sync builtins. This
  contradiction is deliberate — at least one real corpus header
  depends on each branch. The `__sync_*` builtins are not actually
  implemented in codegen (link error if reached); this works because
  no in-scope program reaches them.

- **`__has_attribute` / `__has_builtin` consult a hardcoded allowlist**
  (Q13). The lists may go stale relative to actual parser/codegen
  support. Out-of-allowlist queries return 0.

- **`line_delta` on tokens emitted from macro body expansion** retains
  the value from the body's `copy_token` source, not the macro
  invocation site (Q12). This is preserved for behavioral
  compatibility; documented as an inconsistency rather than a feature.

- **Wide / UTF-16 / UTF-32 string literal types** collapse to `char[N]`
  (inherited from tokenizer; see `01_tokenizer.md` §13).

- **`fopen` leak in include-path probing on `main` is fixed** (Q2).
  The new implementation uses `access(R_OK)`. This is one of the few
  places the swap-out implementation deliberately diverges from
  `main`'s behavior; the difference is not observable from any test
  but the spec records it explicitly.

---

## 14. Validation criteria for the swap-out implementation

A re-implementation of the preprocessor derived from this spec is
considered correct when all of the following hold:

1. **Preprocessed-output equivalence** on the validation corpus:
   `scripts/validate_preprocessor.sh ncc-v2 ncc2` exits 0 with
   `PASS=N/N FAIL=0` across the entire corpus (`src/*.c`,
   `tests/sqlite/sqlite3.c`, `tests/sqlite/test_sqlite.c`,
   `tests/regression/*.c`, optionally `tests/cpython/Python-3.12.3/
   Python/ceval.c`).

   Per-file test: `ncc-v2 -E file.c` and `ncc2 -E file.c` produce
   byte-identical output. Both compilers receive the same `-D`/`-I`
   flags. Per-file overrides for sqlite (`-DSQLITE_MEMORY_BARRIER=`)
   and cpython (`-DPy_BUILD_CORE` etc.) are inlined in the harness.

2. **Torture pass rate unchanged**: `tests/torture/run.sh` reports
   the same `PASS=` count as the Phase-1-closed baseline (964 of 995,
   100% on the non-skip set).

3. **Bootstrap fixed-point**: `scripts/bootstrap_validate.sh` exits 0
   (stage1 == stage2 by md5).

4. **Real-program builds unchanged**: `tests/sqlite/build.sh`,
   `build_doom_ncc2.sh`, and `tests/cpython/build.sh` succeed,
   matching the Day-0 baseline (sqlite 20/20 SQL tests, doom 83/83
   files compile + link, cpython 153/153 files compile + Python
   runtime tests pass).

5. **New regression tests pass** (Q17 list — minimum: stringize/no-rescan,
   painter's rule, paste empty arg, zero object macro, `__VA_OPT__`).
   These are added under `tests/regression/` alongside the spec, run
   against the current preprocessor first to define the contract,
   then against the reimpl.

A failure on any of these reverts the swap-out commit per the
phase-discipline rule.

---

## 15. Out of scope

The following are **not** part of Phase 2:

- **`-target elf` predefined macros**: `__ELF__`, `__ARM_ARCH`,
  `__ARM_ARCH_8A__`, `__ARM_PCS_AAPCS64` (added on `main` in commits
  `4ed0320` and `8fe8dda`). These are not present in swap-out's
  `preprocess.c` today and are deferred to Phase 5 (codegen audit),
  where they will land alongside the ELF target-output and `__sync_*`
  builtin work. Per Q18, the spec leaves a hook
  (`init_macros(target)`) so Phase 5 can add `TARGET_ELF` dispatch
  without restructuring.

- **`-E` line markers** (clang-style `# N "filename" 1 2 3 4`): not
  emitted by ncc; spec describes the current minimal `-E` output
  format only. Cross-compiler comparison (ncc vs clang) requires
  separate header-alignment work and is not blocked on Phase 2.

- **Tokenizer behavior**: per `01_tokenizer.md`, owned by Phase 1.
  The preprocessor consumes tokenizer output and relies on the
  tokenizer's `at_bol` and `has_space` invariants but does not
  redefine them.

- **Type system**: per Phase 3. The preprocessor reads `tok->ty` only
  to recognize string types in `read_include_filename` and integer
  types in `#line`; the type system itself is `type.c`'s domain.

- **Parser**: per Phase 4. The preprocessor produces the token stream
  the parser consumes; no syntactic analysis happens here.

- **Driver / argv parsing**: per Phase 6. `init_macros`,
  `add_include_path`, `define_macro`, and `undef_macro` are called
  from `main.c`; how `main.c` parses its argv to make those calls is
  out of scope.

---

## Peeks taken (audit trail)

This section will list every time during spec authoring that source
files were consulted directly (rather than via the inventory). Empty
means the spec was derived from inventory + standard alone.

- **(none yet — skeleton was drafted from inventory only.)**

When the algorithmic core (§5–§12) is filled in, each peek will be
logged here with the section it informed. This makes the spec's
independence (or lack thereof) auditable per the project's "weak
claim" framing (no chibicc bytes in the final code; spec is honest
about its derivation path).
