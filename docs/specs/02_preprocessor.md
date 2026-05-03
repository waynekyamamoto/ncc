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

**Current status (2026-05-03):** §1–§7, §13–§15 drafted.
§8–§12 remain stubbed; next batch covers §8–§10.

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

`preprocess2` is the central walk-and-dispatch loop that powers the
preprocessor. It is called once per translation unit by `preprocess`
(§4) and recursively by `eval_const_expr` (§9, to expand macros
inside `#if` expressions) and by `read_include_filename` (§10.4, to
expand a macro that produces a header filename).

### 5.1 Algorithm

Walk the input token stream left-to-right. At each token, in order:

1. **Try macro expansion.** Call `expand_macro(&tok, tok)` (§6.2). If
   it returns true, an expansion happened and `tok` was advanced;
   continue the loop.
2. **Non-directive token.** If `!is_hash(tok)` (i.e., not at a `#`
   that starts a line per §2.1's `at_bol` rule), append `tok` to the
   output stream, write `tok->line_delta = tok->file->line_delta`
   onto it (the per-token snapshot per §2.1), advance, and continue.
3. **Directive line.** Otherwise, dispatch on the directive name
   (the token after `#`). Per-directive handlers consume the rest
   of the directive's logical line and return `tok` positioned at
   the next directive or non-directive token. Each handler is
   responsible for not appending the directive's own tokens to the
   output.
4. **Null directive.** A `#` immediately followed by a newline (i.e.,
   the next token is `at_bol`) is a no-op; advance and continue.
5. **Unknown directive.** Anything else raises `error_tok(tok,
   "invalid preprocessor directive")`.

When the loop exits (`tok->kind == TK_EOF`), append the EOF token to
the output stream and return.

### 5.2 Directive table

The directive name (the token after `#`) selects:

| Directive | Handler | Spec section |
|---|---|---|
| `include` | inline include resolver | §10.1 |
| `include_next` | inline include resolver, alternate search | §10.3 |
| `define` | `read_macro_definition` | §6.1 |
| `undef` | inline `undef_macro` call | §6.1 |
| `if` | `eval_const_expr` + `push_cond_incl` + maybe `skip_cond_incl` | §8.1, §9 |
| `ifdef` / `ifndef` | direct `hashmap_get2` + `push_cond_incl` + maybe `skip_cond_incl` | §8.1 |
| `elif` | `eval_const_expr` if not yet included; otherwise skip | §8.2 |
| `else` | toggle context, skip if already included | §8.2 |
| `endif` | pop `cond_incl` stack | §8.3 |
| `line` | recursive `preprocess2` + `convert_pp_tokens` to read the line number; update `file->line_delta` and optionally `display_name` | §12.1 |
| `pragma` | inline `#pragma once` recognition (stub); skip rest of line | §12.2 |
| `error` | `error_tok` (terminates) | §12.3 |
| `warning` | `warn_tok` + skip rest of line | §12.3 |

The dispatch is implemented as a sequence of `if (equal(tok,
"...")) { ... continue; }` checks; the order matters only insofar as
each handler must position `tok` correctly before the next iteration.

---

## 6. Macro expansion

### 6.1 Macro definition: `#define`, `#undef`

`#define` calls `read_macro_definition(&tok, tok->next)`. That
function:

1. Reads the macro name (one identifier or keyword).
2. Determines whether the macro is **function-like** by the rule:
   `!tok->has_space && equal(tok, "(")` after the name — i.e., a
   `(` immediately following the name (no whitespace) marks
   function-like; otherwise object-like. (Per Q7, the spec uses
   the C11 phrasing: "a left parenthesis with no intervening white
   space"; the implementation uses `has_space` to detect it.)
3. For function-like, parses the parameter list:
   - Comma-separated identifiers become `MacroParam` entries
     (§2.3).
   - A trailing `...` makes the macro variadic with
     `va_args_name = "__VA_ARGS__"` (C99 standard form).
   - A trailing `name...` (GNU named-variadic extension) sets
     `va_args_name = "name"`.
4. Reads the body: tokens up to the next `at_bol`, terminated with
   `TK_EOF`.
5. Calls `add_macro(name, is_objlike, body)` to register the macro.

A redefinition is silent: the new entry overwrites the old in the
hashmap. (No "already-defined" warning.)

`#undef` reads one identifier or keyword and calls
`undef_macro(name)`, which removes the entry from the hashmap.
Undefining a macro that does not exist is silently a no-op.

### 6.2 `expand_macro(Token **rest, Token *tok)` — single-step expansion

Returns `true` if `tok` was a macro invocation that expanded;
`false` otherwise. On `true`, `*rest` points at the start of the
expansion result (which has the original `tok->next`-and-following
appended after the expansion body).

Algorithm:

1. **Hideset check.** If `hideset_contains(tok->hideset, tok->loc,
   tok->len)`, the macro name appears in the token's hideset —
   expansion is suppressed. Return `false`.
2. **Lookup.** `find_macro(tok)` returns the `Macro *` (or `NULL`).
   `find_macro` accepts both `TK_IDENT` and `TK_KEYWORD` (per the
   §2.1 note). If `NULL`, return `false`.
3. **Builtin handler dispatch.** If `m->handler != NULL`, call
   `m->handler(tok)` to produce the replacement token list. Splice
   it in: `(*rest)->next = tok->next`. Return `true`. (See §6.5
   for the handler implementations.)
4. **Object-like macro** (`m->is_objlike == true`):
   - Construct the body's hideset:
     `hs = hideset_union(tok->hideset, new_hideset(m->name))`.
   - `body = add_hideset(m->body, hs)` — copies every body token
     and stamps the hideset onto each copy.
   - Walk the body up to its `TK_EOF`; on each token, set
     `t->origin = tok`. (The appended `tok->next` tail is **not**
     touched — origin is body-only; see §13.)
   - `body = append(body, tok->next)` — link the body to the
     remaining input.
   - `*rest = body`. Return `true`.
5. **Function-like macro** (`m->is_objlike == false`):
   - Require `equal(tok->next, "(")`. If not, return `false` (this
     is not a function-like invocation; the identifier is left in
     the stream as-is). This is the rule that lets `FOO` (without
     `(...)`) appear in source even when `FOO` is a function-like
     macro.
   - Save `macro_tok = tok`. Call `read_macro_args(&tok, tok,
     m->params, m->va_args_name)` (§6.3). This advances `tok` past
     the closing `)`, and returns the argument list `args`. Save
     `rparen = tok` (the position of the closing `)`).
   - **Pre-expand** each argument once: for every `ap` in `args`,
     `ap->expanded = preprocess2(copy_token_list(ap->tok))`. This
     is the C standard's "rescan before substitution" pass; the
     pre-expanded form is consumed by §6.4 for regular argument
     substitution.
   - **Painter's rule for hideset** (Q9 / ISO C11 §6.10.3.4):
     `hs = hideset_intersection(macro_tok->hideset, rparen->hideset)`,
     then `hs = hideset_union(hs, new_hideset(m->name))`.
   - `body = subst(m->body, args)` (§6.4).
   - `body = add_hideset(body, hs)`.
   - Walk the body up to `TK_EOF`; on each token, set
     `t->origin = macro_tok`.
   - `body = append(body, tok)` (where `tok` now points just past
     the `)`).
   - `*rest = body`. Return `true`.

The painter's-rule asymmetry — object-like uses
`union(tok->hideset, {name})` directly; function-like uses the
intersection of the macro-name and rparen hidesets — is the C
standard's correctness condition for nested function-like calls.

### 6.3 `read_macro_args` — argument collection

Called with `tok` positioned at the macro name (the `(` is
`tok->next`). Returns a `MacroArg` list of length
`len(params) + (va_args_name ? 1 : 0)`.

Algorithm:

1. Skip the macro name and `(`. Save `start` for error reporting.
2. **Per fixed parameter** (`pp` walks `params`):
   - For all but the first, skip the leading `,`.
   - Allocate a new `MacroArg`, set `arg->name = pp->name`.
   - Read tokens into `arg->tok` until a `,` or `)` is reached at
     nesting level 0. Track parenthesis nesting (`level++` on `(`,
     `level--` on `)`) to allow commas inside nested parentheses
     within an argument.
   - **Conditional directives inside argument lists** are handled
     by calling `handle_pp_directive_in_arg(tok)` whenever
     `is_hash(tok)` matches inside the loop. This shares the
     module's `cond_incl` stack with the main loop. Other
     directives (`#define`, `#undef`, etc.) inside an argument
     list cause the rest of the line to be skipped.
   - End each argument with a `TK_EOF` sentinel.
3. **Variadic tail** (when `va_args_name != NULL`):
   - If at least one fixed parameter was consumed and the current
     token is `,`, skip it (separator between fixed and variadic).
     If no fixed parameters were consumed, any leading `,` is
     part of the variadic tail (e.g., empty first variadic arg).
   - Allocate `MacroArg` with `is_va_args = true`. Read tokens
     until the matching `)` at nesting level 0, including any
     commas as part of the variadic tokens.
4. Verify the closing `)` and return.

Errors: unclosed argument list raises `error_tok(start, "unclosed
macro argument list")`.

### 6.4 `subst(body, args)` — substitution into body

Walks the macro body left-to-right and produces the substituted
token list. Returns a new linked list (every input token is
copied or replaced).

For each input `tok`:

1. **`#param` (stringize).** If `equal(tok, "#")`:
   - `find_arg(args, tok->next)` must return a non-NULL `arg`;
     otherwise `error_tok(tok->next, "'#' is not followed by a
     macro parameter")`.
   - Append `stringize(tok, arg->tok)` (§7.1) — note this uses
     the **raw** `arg->tok`, not the pre-expanded `arg->expanded`,
     per C11's "no rescan before stringization" rule.
   - Advance past `#` and the parameter (two tokens).
2. **`tok ## ...` (paste, possibly chained).** If `equal(tok->next,
   "##")`:
   - **Set up the LHS.** If `tok` is a parameter (`find_arg`
     succeeds): if its raw arg is empty, skip both the empty arg
     and the `##` (placemarker rule, §7.3); otherwise copy the arg
     tokens into the output. If `tok` is not a parameter, copy
     `tok` itself into the output.
   - **Loop on the RHS** (handles chained `A##B##C##D`):
     - The next token after `##` is the RHS. If it's a parameter:
       - If RHS is empty AND the LHS just produced a `,`:
         **GNU `, ## __VA_ARGS__` extension** — delete the
         trailing comma from the output.
       - If RHS is non-empty AND the LHS just produced a `,`:
         GNU non-empty case — keep the comma, copy the RHS arg
         tokens directly (no paste).
       - Otherwise (non-empty RHS, non-comma LHS): paste the
         current output's tail with the first RHS token via
         `paste(cur, rhs->tok)` (§7.2), then copy any remaining
         RHS arg tokens.
     - If the RHS is not a parameter: paste with `paste(cur, tok)`
       and advance.
     - Advance past the RHS. If the next token is another `##`,
       skip it and continue the loop. Otherwise break.
3. **`__VA_OPT__(...)`.** If `equal(tok, "__VA_OPT__") &&
   equal(tok->next, "(")`:
   - Find the variadic argument among `args` (the one with
     `is_va_args == true`).
   - If the variadic arg is non-empty (`va->tok->kind != TK_EOF`),
     copy the tokens between `(` and the matching `)` into the
     output (respecting nested parentheses).
   - If the variadic arg is empty, skip those tokens.
   - Advance past the closing `)`.
4. **Regular argument substitution.** If `find_arg(args, tok)`
   succeeds:
   - Copy the **pre-expanded** `arg->expanded` tokens into the
     output (one round of preprocess2 has already been applied
     per §6.2).
   - The first copied token's `has_space` is overwritten with
     `tok->has_space` — i.e., the spacing visible at the
     parameter-reference site governs how the expansion looks.
     Subsequent tokens keep their inherited spacing.
   - Advance past the parameter token.
5. **Plain token.** Copy `tok` into the output, advance.

When the body's `TK_EOF` is reached, terminate the output with a
fresh `TK_EOF` (carrying the position from the last consumed
token) and return.

### 6.5 Builtin handler macros

Five macros have `Macro->handler` set instead of a body:
`__FILE__`, `__LINE__`, `__COUNTER__`, `__TIMESTAMP__`,
`__BASE_FILE__`. When `expand_macro` sees `m->handler != NULL`, it
calls the handler and splices the result into the stream (§6.2).

| Macro | Handler | Output |
|---|---|---|
| `__FILE__` | `file_macro` | `"display_name"` of the deepest non-`origin` token (i.e., the source file at the macro use site, walking through any expansion-origin chain) |
| `__LINE__` | `line_macro` | `(line_no + file->line_delta)` of the same use-site token, formatted as decimal |
| `__COUNTER__` | `counter_macro` | the value of a static monotonic `int` (starts at 0), then increments |
| `__TIMESTAMP__` | `timestamp_macro` | the literal string `"Unknown"` (see §13) |
| `__BASE_FILE__` | `base_file_macro` | `"base_file"` (the original source file passed to the driver, before any `#include`); empty string if `base_file` is `NULL` |

Each handler's output is produced by `format`-building a small
string and calling `tokenize(new_file(...))` to lex it. The
handler returns a `Token *` of length 1 (plus a `TK_EOF`); the
caller in `expand_macro` then splices `tok->next` after it.

The origin-chain walk (`file_macro` and `line_macro`) is what makes
`__FILE__` and `__LINE__` report the **macro use site**, not the
deepest expansion. Other handlers don't walk because they don't
depend on source location.

---

## 7. Stringize (`#`) and paste (`##`)

### 7.1 `#` operator (`stringize(hash, arg)`)

Builds a string literal token from an argument's raw token list
(`arg->tok`, not `arg->expanded` — see §6.4).

Algorithm:

1. **First pass — concatenate.** For each token `t` in the arg
   list (until `TK_EOF`):
   - If `t != arg` (not the first token) AND `t->has_space` is
     true, write a single space.
   - Write `t->loc[0..len)` (the source-text spelling of the
     token).
   - **Note:** the first token's own `has_space` is *not*
     consulted, so `# x` (space before x) and `#x` produce the
     same string. Documented in §13.
2. **Second pass — escape and quote.** Wrap the result in `"..."`,
   escaping every `\` and `"` with a leading backslash. (No other
   escapes — newlines, tabs, etc. are not escaped because the
   input tokens cannot contain them; the tokenizer would have
   split on them.)
3. **Re-tokenize.** Call `tokenize(new_file(hash->file->name,
   hash->file->file_no, escaped_buf))` to produce a new `TK_STR`
   token. Return it.

The re-tokenization uses the source position of the `#` token, so
the resulting `TK_STR` reports as having come from the directive
file (relevant for error messages downstream).

### 7.2 `##` operator (`paste(lhs, rhs)`)

Concatenates the source spellings of two tokens and re-lexes the
result. Errors if more than one token results.

Algorithm:

1. Build `buf = format("%.*s%.*s", lhs->len, lhs->loc, rhs->len,
   rhs->loc)`.
2. `tok = tokenize(new_file(lhs->file->name, lhs->file->file_no,
   buf))`.
3. If `tok->next->kind != TK_EOF` (i.e., the result tokenized to
   more than one token), `error_tok(lhs, "pasting forms \"%s\", an
   invalid token", buf)`.
4. Return `tok` (a single token plus its `TK_EOF`).

### 7.3 Placemarker rule (empty argument paste)

Per C11 §6.10.3.3 paragraph 2: "If either operand is a
placemarker pp-token, the result is the other operand." This is
**not** implemented in `paste` itself — `paste` would error on an
empty buffer because the result would tokenize to zero tokens
(failing the "exactly one token" check).

Instead, the placemarker rule is implemented in `subst` (§6.4)
during the `##` handling:

- If the LHS is a parameter and its raw arg is empty: skip both
  the empty arg and the `##` and continue with the next token.
  (The subsequent token becomes the LHS of the next paste, if
  any.)
- If the RHS is a parameter and its raw arg is empty (and the LHS
  is not a `,`): the paste does nothing (the LHS already in the
  output stays, the empty RHS contributes nothing). Code path:
  the inner `for (;;)` loop's first branch falls through without
  appending or pasting when `rhs->tok->kind == TK_EOF`.

The placemarker rule applies only to **literally empty**
arguments at the call site (e.g., `CONCAT(foo,)`). It does **not**
apply to arguments that *expand to nothing*: per the rescan rule
(§2.3), `##` operates on `arg->tok` (raw), not `arg->expanded`. So
`CONCAT(foo, EMPTY)` where `EMPTY` is `#define EMPTY` produces
`fooEMPTY` (paste of literal tokens), not `foo` (placemarker).
This subtlety is captured in `tests/regression/19_paste_empty_arg.c`.

### 7.4 Chained paste

`A ## B ## C ## D` is supported by the inner `for (;;)` loop in
`subst`'s `##` case. After the first paste produces `cur`, the
loop checks whether the next input token is another `##` and
continues pasting `cur` against the next RHS, until the chain
ends. Each link can independently invoke the placemarker rule.

### 7.5 GNU `, ## __VA_ARGS__` extension

A widespread GCC extension predating C99's `__VA_OPT__`: a `,`
immediately before `## __VA_ARGS__` is **deleted** when
`__VA_ARGS__` is empty, and **preserved** when `__VA_ARGS__` is
non-empty. Used to write variadic logging macros where a trailing
comma would be wrong:

```c
#define LOG(fmt, ...) printf(fmt, ## __VA_ARGS__)
LOG("hi")        // -> printf("hi")          (comma deleted)
LOG("%d", 42)    // -> printf("%d", 42)      (comma kept)
```

Implementation in `subst`'s `##` loop:

- When the RHS is the variadic parameter:
  - If empty AND the current `cur` is a `,`: delete the comma
    from the output (walks the output list to find the previous
    node, sets its `next` to `NULL`, sets `cur = prev`).
  - If non-empty AND `cur` is a `,`: keep the comma, copy the
    RHS arg tokens directly without pasting.

C23's `__VA_OPT__` (§6.4) is the standard replacement; both
mechanisms coexist in this preprocessor and the GNU extension is
preserved for behavioral compatibility with the corpus.

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

- **`pragma_handler` callback is registered but never invoked.** The
  public `set_pragma_handler` API exists (`cc.h`) and stores the
  function pointer in a module-level `static`, but `preprocess2`'s
  `#pragma` branch never calls it. Currently all non-standard
  pragmas (everything except `#pragma once`) are silently absorbed
  by the rest-of-line skip. The new implementation matches this
  behavior — the public API is preserved for binary compatibility
  but is effectively dead. **Q22 (raised during §5–§7 drafting):**
  should the new implementation invoke the registered handler, or
  should `set_pragma_handler` be removed from the public API
  entirely? Recommended: invoke it (the API is documented; dead
  infrastructure that promises a callback is worse than working
  code).

- **`#error` and `#warning` emit empty diagnostic messages.** Both
  directives call `error_tok` / `warn_tok` with `""` as the format
  string, so the diagnostic is just the source-line caret with no
  descriptive text. The directive's rest-of-line tokens (which by
  C standard convention are the message) are not emitted. Real
  compilers (gcc, clang) include the message tokens in the
  diagnostic. **Q23 (raised during §5–§7 drafting):** preserve the
  empty-message bug for behavioral compat, or fix to emit the
  message tokens? Recommended: fix. The bug is plainly broken (no
  one *wants* `#error "this is broken"` to say nothing); fixing it
  cannot regress any working program.

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
