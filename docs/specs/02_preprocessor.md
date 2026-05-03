# ncc Preprocessor — English Specification (Phase 2)

This document is the behavioral contract for ncc's preprocessor on the
`swap-out` branch. Phase 2 of the chibicc swap-out replaces the
inherited `src/preprocess.c` (1,798 lines, chibicc lineage) with a
reimplementation written from this document. Both the current code
and the reimplementation must satisfy the contract; mechanical
validation (`scripts/validate_preprocessor.sh`, bootstrap fixed-point,
the gcc torture suite, real-program builds) verifies behavioral
equivalence.

This is **not** a from-scratch C preprocessor design — that work lives
in a separate project. Here, the goal is byte-for-byte preprocessed-
output equivalence with `main`'s preprocessor, achieved through a
spec-mediated rewrite that removes chibicc-lineage code from `src/`.

When the implementation and this document disagree, the document is
authoritative; bugs in the implementation are fixed against the
document. When the document and `main`'s observable behavior disagree,
this document is wrong and must be updated, unless the divergence is
recorded in §13 as intentional.

**Phase 2 scope:** macOS / AArch64 only. The `-target elf` predefines
from `main` commits `4ed0320` and `8fe8dda` (`__ELF__`, `__ARM_ARCH`,
`__ARM_ARCH_8A__`, `__ARM_PCS_AAPCS64`) are explicitly **out of scope**
and are deferred to Phase 5; see §15.

**Current status (2026-05-03):** §1, §2, §3, §4, §13–§15 drafted.
§5–§12 (the algorithmic core) remain stubbed pending the §2–§4
review pass.

---

## 1. Scope

The preprocessor takes a tokenized translation unit (a `Token *` list
produced by `tokenize` + `tokenize_file`) and produces a transformed
`Token *` list with all preprocessor directives executed and all macro
invocations expanded. Its output is the input to the parser.

**Inputs:**
- A `Token *` list, terminated by `TK_EOF`. Tokens carry the fields
  established by the tokenizer (see `01_tokenizer.md` §3).
- Module state established before `preprocess()` is called: include
  paths (from `add_include_path`), macros (from `init_macros` for
  predefined; from `define_macro` / `undef_macro` for command-line
  `-D` / `-U`), pragma handler (from `set_pragma_handler`).

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
    that pasting can build new preprocessing-numbers).

**Interface to the rest of the compiler:**
- `preprocess(Token *tok)` — top-level entry. Called once per
  translation unit by the driver after `tokenize_file` returns the
  raw token stream. Returns the transformed stream.
- `init_macros(int target)` — called once at compiler startup to
  register predefined macros and handler macros for the given
  target. Phase 2 supports `target = TARGET_MACOS`; Phase 5 will add
  `TARGET_ELF` (per Q18).
- `define_macro(char *name, char *body_text)` — register a macro at
  module scope. Used by `init_macros` and by the driver for `-D`.
- `undef_macro(char *name)` — remove a macro at module scope. Used
  by the driver for `-U`.
- `add_include_path(char *path)` — append a path to the search list.
  Used by the driver for `-I`.
- `set_pragma_handler(PragmaHandler *fn)` — installs a callback the
  preprocessor invokes for `#pragma` lines it does not handle
  internally.

**Two passes the preprocessor is *not* responsible for:**
- **Tokenization** (per `01_tokenizer.md`). The preprocessor consumes
  tokenizer output and relies on the tokenizer's invariants for
  `at_bol` and `has_space`.
- **Parsing.** The preprocessor produces a fully-expanded token
  stream; the parser sees no `#`-prefixed directives and no
  unexpanded macros.

---

## 2. Data model

This section describes the data structures the preprocessor operates
on. Field-level details are given for each struct; semantic
operations are spelled out where the data structure is non-obvious.

### 2.1 Token fields consumed and produced

The preprocessor reads and (in some cases) writes the following
fields of `Token` (defined in `cc.h`; see `01_tokenizer.md` §3 for
the tokenizer's contract):

| Field | Read | Written | Notes |
|---|---|---|---|
| `kind` | yes | `TK_EOF` (sentinels), `TK_PP_NUM` (`defined`/`__has_*` results), `TK_NUM` (via `convert_pp_tokens` in the final pass) | `find_macro` accepts both `TK_IDENT` and `TK_KEYWORD` (e.g., `#define inline __forceinline` is legal) |
| `next` | yes | yes | every list-building helper writes |
| `loc`, `len` | yes | inherited via `copy_token`; `read_const_expr` overwrites with static `"0"`/`"1"` for `defined`/`__has_*` results | the static-string overwrite does not point into the source buffer |
| `val`, `fval`, `ty`, `str` | `val` for `TK_NUM` integer-constant evaluation; `ty`/`str` for `TK_STR` filenames in `#include`; `ty->kind` for `TK_NUM` in `#line` | inherited via `copy_token`; written by `convert_pp_tokens` | floats are not used in `#if` evaluation |
| `file` | yes (`__FILE__`, `dirname()` for `#include "..."`, `#line`) | `file->line_delta`, `file->display_name` written by `#line` handler | per-file mutable state, not per-token |
| `line_no` | `__LINE__` | inherited | set by the tokenizer's `add_line_numbers` pass |
| `line_delta` | not read by preprocessor logic | written on every output token in the main directive-dispatch loop (see §5) | per-token snapshot of `file->line_delta`; spec note in §13 about what this means on tokens that come out of macro expansion |
| `at_bol` | `is_hash` (directive detection); `skip_line`; `copy_line`; `read_include_filename`; `read_macro_args`; `skip_cond_incl*` | preprocessor does not modify directly (only via re-tokenization inside `stringize`/`paste`) | tokenizer-set value is load-bearing |
| `has_space` | `read_include_filename` (`<...>` token concatenation); `read_macro_definition` (function-like detection per §6.1); `subst` (first-token-of-arg propagation) | `subst` writes `cur->has_space = tok->has_space` for the first token of a substituted argument; otherwise inherited via `copy_token` | tokenizer-set value is load-bearing for function-like detection (Q7) |
| `hideset` | `expand_macro` (suppression check); `add_hideset` (union) | `add_hideset` (after `copy_token`) | **must be tokenizer-`NULL` initially** — `hideset_union` treats NULL as the empty hideset |
| `origin` | `file_macro` / `line_macro` (origin-chain walk to source token); `error_tok` (macro-use-site walk) | `expand_macro` writes `t->origin = tok` (object-like) or `t->origin = macro_tok` (function-like) on body tokens; appended tail is not touched (see §6.2) | **must be tokenizer-`NULL` initially** — origin-chain walk terminates on NULL |

The two `NULL`-initial-value reliances above are invariants the
tokenizer must maintain. They are documented in `01_tokenizer.md` §3
under the same name.

### 2.2 `Macro`

Represents one entry in the macro table. One `Macro` per defined
name; `undef_macro` removes the entry from the table entirely (no
"deleted" flag — use `hashmap_delete` semantics).

| Field | Type | Purpose |
|---|---|---|
| `name` | `char *` | macro name, used as the hashmap key |
| `is_objlike` | `bool` | `true` for object-like (`#define X body`); `false` for function-like (`#define X(p) body`) |
| `params` | `MacroParam *` | parameter list head (function-like only; `NULL` for object-like) |
| `va_args_name` | `char *` | name of the variadic parameter, or `NULL` if not variadic. Two forms: `__VA_ARGS__` (C99 standard, set when the parameter is `...`) or a named variadic (GNU extension, set when the parameter is `name...`) |
| `body` | `Token *` | replacement-list token stream, terminated by `TK_EOF` |
| `handler` | `MacroHandlerFn *` | non-`NULL` for builtin handler macros (`__FILE__`, `__LINE__`, `__COUNTER__`, `__TIMESTAMP__`, `__BASE_FILE__`); when set, `expand_macro` calls the handler instead of substituting `body` |

**Implementation note:** The current `src/preprocess.c` carries an
`is_locked` field (line 41) that is declared but never read. The
recursive-expansion guard is the hideset, not a per-macro lock. The
new implementation should drop `is_locked`.

### 2.3 `MacroParam` and `MacroArg`

`MacroParam` is the static description of a function-like macro's
parameter list, established at `#define` time:

| Field | Type | Purpose |
|---|---|---|
| `next` | `MacroParam *` | linked-list link |
| `name` | `char *` | parameter name |

`MacroArg` is the dynamic per-invocation argument record, built at
each call site:

| Field | Type | Purpose |
|---|---|---|
| `next` | `MacroArg *` | linked-list link |
| `name` | `char *` | parameter name (matched from `MacroParam`) |
| `is_va_args` | `bool` | `true` for the variadic tail (collected under `va_args_name`) |
| `tok` | `Token *` | raw argument tokens, not pre-expanded, terminated by `TK_EOF`. Used by `#` (stringize) and `##` (paste) per C11 §6.10.3.1. |
| `expanded` | `Token *` | argument tokens after one round of `preprocess2`. Used by regular argument substitution. |

The `tok` vs `expanded` distinction is the C standard's "rescan
before substitution, but not before stringization or pasting" rule;
its consequences for §6.4 (`subst`) and §7 (stringize/paste) must be
preserved exactly.

### 2.4 `Hideset`

A linked-list set of macro names used to prevent recursive expansion
per the C11 painter's-rule semantics (Prosser 1984; ISO C11
§6.10.3.4).

```
struct Hideset {
    Hideset *next;
    char *name;
};
```

Operations (described semantically; implementation may choose any
representation that preserves the contract):

- `new_hideset(name)` — singleton hideset containing `{name}`.
- `hideset_union(a, b)` — set union. The current implementation
  concatenates without dedup; duplicates are harmless because
  `hideset_contains` terminates on first match. A deduplicating
  implementation is also correct (Q3).
- `hideset_intersection(a, b)` — set intersection. Used only for
  function-like macros (the painter's rule, §6.2).
- `hideset_contains(hs, name, len)` — membership test.
- `add_hideset(tok_list, hs)` — copy each token in the list and
  union `hs` into each copy's `hideset` field.

A token's hideset is populated only by the preprocessor; the
tokenizer leaves it `NULL`. NULL means "the empty hideset" for all
operations.

### 2.5 `CondIncl`

A stack node tracking one open `#if` / `#ifdef` / `#ifndef` block:

| Field | Type | Purpose |
|---|---|---|
| `next` | `CondIncl *` | stack link |
| `ctx` | enum `{IN_THEN, IN_ELIF, IN_ELSE}` | which clause we're currently inside |
| `tok` | `Token *` | the directive token (for error reporting) |
| `included` | `bool` | `true` once any branch of this `#if` chain has been selected; suppresses subsequent `#elif`/`#else` |

The stack is module-level state (§3); `#if` pushes, `#endif` pops.
After `preprocess()` returns, the stack must be empty; non-empty
indicates an unterminated `#if`, which is a fatal error.

### 2.6 Module-level collections

- **Macro table.** Hashmap keyed by macro name (string), values are
  `Macro *`. Lookups happen on raw `tok->loc[0..len)` to avoid
  copying for the common case.
- **Include-path list.** Ordered list of directory paths, populated
  by `add_include_path`. Searched left-to-right by
  `search_include_paths` (§10.4).
- **Pragma handler.** Optional callback set by `set_pragma_handler`;
  invoked for `#pragma` directives the preprocessor doesn't handle
  internally (§12.2). May be `NULL` (no callback installed).
- **Counter.** Monotonic `int` for `__COUNTER__`; starts at 0,
  incremented on each `__COUNTER__` expansion.
- **Base file pointer.** `char *` set once at the start of compilation
  to the original source file (before any `#include`); read by
  `__BASE_FILE__`.

How these collections are physically represented (module-level
`static` globals vs a context struct passed around) is left to the
implementation per §3.

---

## 3. Module state

The preprocessor needs persistent state across the entry points
listed in §1 (`init_macros`, `add_include_path`, `define_macro`,
`undef_macro`, `set_pragma_handler`, `preprocess`). The logical
state is enumerated in §2.6.

The spec deliberately does **not** prescribe whether this state
lives in module-level `static` globals (the current implementation)
or in an explicit `Preprocessor` context struct passed to each entry
point. Either is conformant. Phase 2's reimplementation is free to
preserve the global structure (matches `main`, minimal scope); a
later phase (the driver swap-out, Phase 6) may refactor toward an
explicit context if the driver's per-translation-unit needs make
that natural.

### 3.1 Lifecycle

The driver calls preprocessor entry points in this order, per
translation unit:

1. **Once per process** (or once per `init_macros` invocation): set
   the predefined macros via `init_macros(target)`. This populates
   the macro table with the macOS predefine set (Phase 2 scope) plus
   the five handler macros (`__FILE__`, etc.).
2. **For each `-I` flag** on the driver command line:
   `add_include_path(path)`. Order matters: paths are searched
   left-to-right.
3. **For each `-D name[=body]` flag**: `define_macro(name, body)`
   (the driver constructs the body string, defaulting to `"1"`
   when `=body` is absent).
4. **For each `-U name` flag**: `undef_macro(name)`.
5. **Optionally**: `set_pragma_handler(fn)` if the driver wants to
   handle non-standard pragmas.
6. **Per source file**: tokenize, then `preprocess(tok)`.

The `__DATE__` and `__TIME__` predefines are computed once at
`init_macros` time (a known divergence from per-TU semantics; see
§13).

### 3.2 Lifetime and concurrency

The preprocessor is **not** designed to be re-entrant or thread-safe.
The state described above is shared mutably across calls. A
multi-TU driver invocation (a future Phase 6 capability) must either
serialize translation units or refactor the state into an explicit
context.

---

## 4. Top-level entry point: `preprocess(Token *tok)`

The function `preprocess` is called once per translation unit. Its
body is structurally simple; the substance lives in `preprocess2`
(§5), which is the directive-dispatch loop.

**Algorithm:**

1. Reset the per-translation-unit token counter (`pp_token_count = 0`).
   This is exposed for performance reporting from the driver.
2. Call `preprocess2(tok)` to walk the input stream and produce the
   transformed stream (§5).
3. Verify the conditional-inclusion stack is empty. If not, an
   `#if` was opened but never closed; report an error via the
   directive's stored `tok` (`error_tok`).
4. Walk the resulting stream and call `convert_pp_tokens(result)` to
   lower every `TK_PP_NUM` to `TK_NUM` with a real value, type, and
   (for floats) `fval`. This is deferred until after macro expansion
   because macro pasting (`##`) can construct new preprocessing
   numbers from existing token spellings.
5. Return the resulting stream.

The function does **not** consume the input list `tok` destructively;
it reads `tok` and produces a new list (with extensive token-level
copying inside `preprocess2`). Callers can re-read the input after
return, though there is no current use case that does so.

---

## 5. Main directive-dispatch loop: `preprocess2(Token *tok)`

> **STUB — to be drafted in batch §5–§7.** This section describes the
> central walk-and-dispatch loop that powers the preprocessor: it
> walks the token stream left-to-right, dispatching `#`-prefixed
> directive lines (`is_hash(tok)`) to per-directive handlers (§6,
> §8, §10, §12) and macro-expanding non-directive tokens via
> `expand_macro` (§6.2). Recursively re-entered by `eval_const_expr`
> to expand macros inside `#if` expressions, and by
> `read_include_filename` to expand a macro that produces a header
> filename.

---

## 6. Macro expansion

> **STUB — to be drafted in batch §5–§7.** Subsections planned:
> 6.1 macro definition (`#define`, `#undef`); 6.2 `expand_macro`
> (single-step expansion with hideset enforcement); 6.3
> `read_macro_args`; 6.4 `subst` (substitution into body, including
> `__VA_OPT__` and GNU `, ## __VA_ARGS__`); 6.5 builtin handler
> macros (`__FILE__`, `__LINE__`, `__COUNTER__`, `__TIMESTAMP__`,
> `__BASE_FILE__`).

---

## 7. Stringize (`#`) and paste (`##`)

> **STUB — to be drafted in batch §5–§7.** Subsections: 7.1
> stringize; 7.2 paste; 7.3 placemarker (empty-arg) rule; 7.4
> chained paste `A##B##C##D`.

---

## 8. Conditional inclusion

> **STUB — to be drafted in batch §8–§10.** Subsections: 8.1
> `#if`/`#ifdef`/`#ifndef`; 8.2 `#elif`/`#else`; 8.3 `#endif`; 8.4
> `skip_cond_incl` (skipping not-taken branches); 8.5
> `handle_pp_directive_in_arg` (conditional directives inside macro
> argument lists).

---

## 9. Constant-expression evaluation (`eval_const_expr`)

> **STUB — to be drafted in batch §8–§10.** Six-step pipeline from
> `read_const_expr` through `convert_pp_tokens` to the recursive-
> descent evaluator. `__has_attribute` / `__has_builtin` allowlist
> consultation. `__has_feature` returns 0 (Q5).

---

## 10. `#include` resolution

> **STUB — to be drafted in batch §8–§10.** Subsections: 10.1
> `#include "..."` (current-dirname-first search); 10.2
> `#include <...>` (`include_paths` only); 10.3 `#include_next`;
> 10.4 `read_include_filename` (three patterns); 10.5
> `search_include_paths` (using `access(R_OK)` per Q2, replacing
> `main`'s leaking `fopen`); 10.6 `include_file` splice semantics
> (Q11: behavior specified, mechanism not).

---

## 11. Predefined macros (`init_macros`)

> **STUB — to be drafted in batch §11–§12.** Full table of the
> macOS predefine set with values, organized by category. `__SCHAR_MAX__`
> listed exactly once (Q1: silent dedup). `-target elf` predefines
> remain out of scope (§15). Target-dispatch hook per Q18.

---

## 12. `#line`, `#pragma`, `#error`, `#warning`

> **STUB — to be drafted in batch §11–§12.** `#line` updates
> `file->line_delta` and optionally `file->display_name`. `#pragma
> once` is a documented stub (Q4). All other pragmas pass to the
> registered handler. `#error` / `#warning` concatenate rest-of-line
> spellings and call `error_tok` / `warn_tok`.

---

## 13. Known divergences from ISO C / GCC

These are intentional simplifications or quirks preserved for
behavioral compatibility with `main`'s preprocessor. Recorded here
so a re-implementer does not "fix" them by accident.

- **`#pragma once` is a no-op stub** (Q4). Re-`#include`ing a file
  with `#pragma once` will include it again. Most modern headers use
  include guards, so the divergence rarely surfaces; some Apple SDK
  headers and third-party libraries may behave differently than
  under clang. Test: `tests/regression/NN_pragma_once.c` (to be
  added per Q17).

- **`__has_feature(X)` always returns 0** (Q5) for every X. Headers
  that gate features via clang's `__has_feature` will take the
  fallback path. For C11 code targeting macOS this is almost always
  the right path; for C++ code it would not be (but ncc does not
  compile C++).

- **`__TIMESTAMP__` returns the literal string `"Unknown"`** (Q6).
  Source-file mtime is not stat'd. Banner-style uses are visibly
  affected; correctness-style uses (rare) take a constant value.

- **`__DATE__` / `__TIME__` are fixed at `init_macros()` call time**,
  not per-translation-unit. If the driver compiles multiple TUs in a
  single invocation, all share the same date/time. Currently a
  non-issue since each TU is a separate ncc process.

- **`__STDC_NO_ATOMICS__` = 1 and `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_*`
  = 1 are both advertised** (Q15). Standards-compliant headers see
  no atomics support; GCC-compat headers see sync builtins. This
  contradiction is deliberate — at least one real-corpus header
  depends on each branch. The `__sync_*` builtins are not actually
  implemented in codegen (link error if reached); this works because
  no in-scope program reaches them.

- **`__has_attribute` / `__has_builtin` consult a hardcoded allowlist**
  (Q13). The lists may go stale relative to actual parser/codegen
  support. Out-of-allowlist queries return 0.

- **`line_delta` on tokens emitted from macro body expansion** retains
  the value from the body's `copy_token` source, not the macro
  invocation site (Q12). Preserved for behavioral compatibility;
  documented as an inconsistency rather than a feature.

- **Wide / UTF-16 / UTF-32 string literal types** collapse to `char[N]`
  (inherited from tokenizer; see `01_tokenizer.md` §13).

- **`fopen` leak in include-path probing on `main` is fixed** (Q2).
  The new implementation uses `access(R_OK)`. This is one of the few
  places the swap-out implementation deliberately diverges from
  `main`'s behavior; the difference is not observable from any test
  but the spec records it explicitly.

- **`-E` placemarker output spacing.** When a macro paste produces
  the placemarker rule (e.g., `int CONCAT(foo,) = 1;` where
  `CONCAT(a,b)` is `a##b`), ncc's `-E` output emits the result
  immediately after the preceding token (`intfoo = 1;`) because the
  resulting `foo` token retains the `has_space=false` it had when
  tokenized inside `(...)`. The token stream is correct (`int` and
  `foo` remain separate tokens, parser handles fine); only the
  pretty-printed `-E` output is affected. Test:
  `tests/regression/19_paste_empty_arg.c`.

- **`is_locked` field on `Macro` is unused and dropped.** The
  current `src/preprocess.c:41` declares `bool is_locked` but never
  reads it. The new implementation drops it; recursive-expansion
  prevention is the hideset.

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
   the same `PASS=` count as the Phase-1-closed baseline (964 of
   995, 100% on the non-skip set).

3. **Bootstrap fixed-point**: `scripts/bootstrap_validate.sh` exits
   0 (stage1 == stage2 by md5).

4. **Real-program builds unchanged**: `tests/sqlite/build.sh`,
   `build_doom_ncc2.sh`, and `tests/cpython/build.sh` succeed,
   matching the Day-0 baseline (sqlite 20/20 SQL tests, doom 83/83
   files compile + link, cpython 153/153 files compile + Python
   runtime tests pass).

5. **Phase-2 regression tests pass**: `tests/regression/run.sh`
   reports `PASS=21+ FAIL=0`. The `17_*` through `21_*` tests
   committed in `a043564` are the minimum; per Q17, additional
   tests for `#pragma once` (Q4 contract), `#include_next`, and
   `handle_pp_directive_in_arg` may be added during Phase 2 dev.

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
  builtin work. The `init_macros(int target)` signature in §1 leaves
  the dispatch hook in place per Q18.

- **`-E` line markers** (clang-style `# N "filename" 1 2 3 4`): not
  emitted by ncc; spec describes the current minimal `-E` output
  format only. Cross-compiler comparison (ncc vs clang) requires
  separate header-alignment work and is not blocked on Phase 2.

- **Tokenizer behavior**: per `01_tokenizer.md`, owned by Phase 1.
  The preprocessor consumes tokenizer output and relies on the
  tokenizer's `at_bol` and `has_space` invariants but does not
  redefine them.

- **Type system**: per Phase 3. The preprocessor reads `tok->ty`
  only to recognize string types in `read_include_filename` and
  integer types in `#line`; the type system itself is `type.c`'s
  domain.

- **Parser**: per Phase 4. The preprocessor produces the token
  stream the parser consumes; no syntactic analysis happens here.

- **Driver / argv parsing**: per Phase 6. `init_macros`,
  `add_include_path`, `define_macro`, and `undef_macro` are called
  from `main.c`; how `main.c` parses its argv to make those calls is
  out of scope.

- **Re-entrancy / thread safety**: per §3.2, the preprocessor is
  designed for single-threaded, sequential single-TU use. Any future
  multi-TU or concurrent driver work must refactor the module state
  into an explicit context.
