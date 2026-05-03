# ncc Preprocessor — Phase 2 Inventory

This is a read-only audit of `src/preprocess.c` on the `swap-out` branch (tip
`ad8f346`).  It is an **inventory**, not a spec.  The spec (`02_preprocessor.md`)
is the user-collaborative next step that comes after this document.

Phase 2 scope: **macOS/AArch64 only.**  The `-target elf` predefined-macro set
from `main` commits `4ed0320` and `8fe8dda` is explicitly **out of scope**; see
§6 for details.

---

## 1. Function inventory

Every function defined in `src/preprocess.c`, with file-line reference, one-line
purpose, and non-trivial call relationships.

### Public (exported via `cc.h`)

| Function | Line | Purpose |
|---|---|---|
| `set_pragma_handler(PragmaHandler *fn)` | 62 | Store a pragma callback; called from `main.c`; the preprocessor calls it for `#pragma` lines it does not handle internally. |
| `add_include_path(char *path)` | 233 | Append one path to the `include_paths` `StringArray`; called from `main.c` for each `-I` flag. |
| `define_macro(char *name, char *buf)` | 193 | Parse and register a built-in or command-line macro.  `name` may contain `(param,...)` for function-like macros; `buf` is the body text.  Calls `tokenize`, `add_macro`. |
| `undef_macro(char *name)` | 225 | Remove a macro from the global `HashMap`.  Called from `main.c` for `-U` flags. |
| `init_macros(void)` | 1201 | Register all predefined macros via `define_macro` / `add_macro`; set handler macros for `__FILE__`, `__LINE__`, `__COUNTER__`, `__TIMESTAMP__`, `__BASE_FILE__`. |
| `preprocess(Token *tok)` | 1791 | Top-level entry point.  Zeroes `pp_token_count`, calls `preprocess2`, checks for unterminated conditional directives (`cond_incl != NULL`), calls `convert_pp_tokens` on the result. |
| `pp_token_count` | 1597 | Public `long` global; incremented once per token visited in `preprocess2`'s main loop.  Used by `main.c` for performance reporting. |

### Static (internal)

**Utility / token manipulation**

| Function | Line | Purpose / called-by |
|---|---|---|
| `is_hash(Token *tok)` | 78 | Returns `tok->at_bol && equal(tok, "#")`.  Called by `preprocess2` and `handle_pp_directive_in_arg`. |
| `skip_line(Token *tok)` | 82 | Advance past remaining tokens on the current line; emit `warn_tok` if any are present.  Called by most directive handlers in `preprocess2`. |
| `copy_token(Token *tok)` | 92 | Deep-copy one `Token` (sets `next = NULL`).  Called by almost every list-building helper. |
| `new_eof(Token *tok)` | 100 | `copy_token` + set `kind = TK_EOF, len = 0`.  Called by `copy_line`, `copy_token_list`, `subst`, `read_macro_args`, `read_const_expr`. |
| `append(Token *tok1, Token *tok2)` | 158 | Copy `tok1` (stop at `TK_EOF`) and link to `tok2`.  Called by `expand_macro`. |
| `copy_line(Token **rest, Token *tok)` | 303 | Copy tokens until `at_bol`, terminate with EOF, advance `*rest`.  Called by `read_const_expr`, `read_macro_definition`, `#line` handler. |
| `copy_token_list(Token *tok)` | 1094 | Deep-copy a token list to `TK_EOF`.  Called by `expand_macro` to clone each macro argument before pre-expansion. |

**Hideset management**

| Function | Line | Purpose |
|---|---|---|
| `new_hideset(char *name)` | 111 | Allocate a single `Hideset` node.  Called by `hideset_union`, `hideset_intersection`, `expand_macro`. |
| `hideset_union(Hideset *hs1, Hideset *hs2)` | 117 | Concatenate two hidesets (no dedup).  Called by `add_hideset`, `expand_macro`. |
| `hideset_contains(Hideset *hs, char *s, int len)` | 128 | Linear scan by `strncmp`.  Called by `expand_macro`. |
| `hideset_intersection(Hideset *hs1, Hideset *hs2)` | 135 | Set intersection (elements of hs1 that also appear in hs2).  Called by `expand_macro` for function-like macros. |
| `add_hideset(Token *tok, Hideset *hs)` | 145 | Copy every token in list, union `hs` into each copy's `hideset`.  Called by `expand_macro`. |

**Macro table**

| Function | Line | Purpose |
|---|---|---|
| `add_macro(char *name, bool is_objlike, Token *body)` | 175 | Low-level `hashmap_put`.  Called by `define_macro`, `read_macro_definition`, `init_macros`. |
| `find_macro(Token *tok)` | 184 | Lookup macro by `tok->loc`/`tok->len`; accepts `TK_IDENT` or `TK_KEYWORD`.  Called by `expand_macro` and `eval_const_expr`. |

**Include resolution**

| Function | Line | Purpose / calls |
|---|---|---|
| `search_include_paths(char *filename)` | 237 | Returns absolute path first if `filename[0]=='/'`; otherwise linear scan of `include_paths` using `fopen` to probe existence.  Called by `preprocess2` (`#include`) and `read_const_expr` (`__has_include`). |
| `search_include_next(char *filename, int start)` | 250 | Same as above but from index `start`; for `#include_next`.  Called by `preprocess2` (`#include_next`). |
| `read_include_filename(Token **rest, Token *tok, bool *is_dquote)` | 260 | Parse `"foo.h"`, `<foo.h>`, or macro-expanded ident; sets `*is_dquote`.  Recursively calls `preprocess2` + itself for the macro-expanded case. |
| `include_file(Token *tok, char *path, Token *filename_tok)` | 1482 | Call `tokenize_file(path)`, walk the result to its `TK_EOF`, overwrite that `TK_EOF` with `tok` to splice the included stream into the current one. |

**Constant-expression evaluation**

| Function | Line | Purpose |
|---|---|---|
| `read_const_expr(Token **rest, Token *tok)` | 319 | `copy_line`, then inline-substitute `__has_attribute`, `__has_builtin`, `__has_feature`, `__has_include`, `__has_include_next` with `0`/`1`, then replace `defined(X)` with `0`/`1`.  Returns the modified token list. |
| `eval_const_expr(Token **rest, Token *tok)` | 513 | Calls `read_const_expr`, then `preprocess2` (macro-expand), replaces remaining identifiers with `0`, calls `convert_pp_tokens`, then `const_expr`. |
| `const_expr(Token **rest, Token *tok)` | 577 | Thin wrapper → `cond`. |
| `cond` | 581 | Ternary operator `?:`. |
| `logor` / `logand` | 594 / 605 | `\|\|` / `&&`. |
| `bitor_` / `bitxor` / `bitand` | 616 / 626 / 636 | `\|` / `^` / `&`. |
| `equality` / `relational` / `shift` | 646 / 662 / 684 | `==`/`!=`, `<`/`<=`/`>`/`>=`, `<<`/`>>`. |
| `add_` / `mul` | 700 / 716 | `+`/`-`, `*`/`/`/`%` (division-by-zero error). |
| `unary` | 741 | Unary `+`, `-`, `!`, `~`. |
| `primary` | 753 | Parenthesised expression; `TK_NUM` (uses `tok->val`); `TK_PP_NUM` (parses with `strtol`, skips integer suffix). |

**Macro expansion**

| Function | Line | Purpose / calls |
|---|---|---|
| `stringize(Token *hash, Token *arg)` | 784 | Build a string literal from an argument token list: two `open_memstream` passes — raw concatenation respecting `has_space`, then escape `\` and `"`.  Re-tokenizes via `tokenize`. |
| `paste(Token *lhs, Token *rhs)` | 816 | `format("%.*s%.*s"...)` + `tokenize`; errors if result is more than one token. |
| `find_arg(MacroArg *args, Token *tok)` | 826 | Linear scan by name length + `strncmp`.  Called by `subst`. |
| `handle_pp_directive_in_arg(Token *tok)` | 837 | Resolve `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif` encountered inside a macro argument token list (e.g. Linux `decompress_inflate.c`).  Other directives: skip the line. |
| `read_macro_args(Token **rest, Token *tok, MacroParam *, char *va_args_name)` | 889 | Consume `(`, collect comma-separated argument token lists (respecting nesting and in-arg directives via `handle_pp_directive_in_arg`), collect variadic tail into `va_args_name` arg. |
| `subst(Token *tok, MacroArg *args)` | 970 | Token-by-token substitution: `#arg` stringification (→ `stringize`), `##`-chained pasting (→ `paste`), GNU `, ## __VA_ARGS__` extension, regular arg substitution (uses pre-expanded `arg->expanded`), `__VA_OPT__`. |
| `expand_macro(Token **rest, Token *tok)` | 1104 | Single expansion step: hideset check, `find_macro`, dispatch handler / object-like / function-like.  Returns `true` if expansion occurred.  Calls `read_macro_args`, `subst`, `add_hideset`, `append`. |

**Built-in macro handlers**

| Function | Line | Purpose |
|---|---|---|
| `file_macro(Token *tmpl)` | 1161 | Walk `tmpl->origin` chain to source token; format `"filename"`; re-tokenize. |
| `line_macro(Token *tmpl)` | 1170 | Walk origin chain; compute `line_no + file->line_delta`; format as decimal. |
| `counter_macro(Token *tmpl)` | 1180 | Increment `counter_val`; format as decimal. |
| `timestamp_macro(Token *tmpl)` | 1185 | Returns the literal string `"Unknown"`. |
| `base_file_macro(Token *tmpl)` | 1191 | Formats the extern `base_file` pointer as a string. |

**Conditional inclusion**

| Function | Line | Purpose |
|---|---|---|
| `push_cond_incl(Token *tok, bool included)` | 1530 | Push new `CondIncl` onto `cond_incl` stack with `ctx = IN_THEN`. |
| `skip_cond_incl(Token *tok)` | 1513 | Advance until an `#elif`, `#else`, or `#endif` at the current nesting level.  Nested `#if`/`#ifdef`/`#ifndef` blocks are skipped via `skip_cond_incl2`. |
| `skip_cond_incl2(Token *tok)` | 1498 | Skip from a nested `#if*` to its matching `#endif` (recursively). |

**Macro definition parsing**

| Function | Line | Purpose |
|---|---|---|
| `read_macro_definition(Token **rest, Token *tok)` | 1540 | Parse `#define` body.  Function-like detected by `!tok->has_space && equal(tok,"(")`.  Handles `...` and named variadic (`name...`).  Calls `add_macro`. |

**Main loop**

| Function | Line | Purpose |
|---|---|---|
| `preprocess2(Token *tok)` | 1598 | Main preprocessor loop: macro expansion, directive dispatch (`#include`, `#include_next`, `#define`, `#undef`, `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`, `#line`, `#pragma`, `#error`, `#warning`), null directive, unknown-directive error.  Recursive: called by `eval_const_expr` and `read_include_filename`. |

---

## 2. Preprocessor responsibilities by topic

### 2.1 Macro expansion

**Object-like macros** — `expand_macro` lines 1119–1128.
Hideset checked first (`src/preprocess.c:1105`).  `find_macro` invoked
(`src/preprocess.c:1108`).  Body tokens cloned with `add_hideset` (union of
token's hideset + macro name); `origin` set to the invoking token
(`src/preprocess.c:1124-1125`).  Body appended to `tok->next`
(`src/preprocess.c:1126`).

**Function-like macros** — `expand_macro` lines 1131–1154.
Requires `tok->next == "("` (`src/preprocess.c:1133`).
`read_macro_args` collects per-param token lists (`src/preprocess.c:1136`).
Each arg pre-expanded via `preprocess2(copy_token_list(ap->tok))`
(`src/preprocess.c:1141`).  Hideset for body = intersection of
`macro_tok->hideset` and `rparen->hideset`, unioned with macro name
(`src/preprocess.c:1144-1145`).  `subst` performs token substitution
(`src/preprocess.c:1146`).  `add_hideset` stamps body
(`src/preprocess.c:1147`).  Origin set to `macro_tok`
(`src/preprocess.c:1149-1150`).

**expand_macro hideset enforcement** — `src/preprocess.c:1105-1106`.
If `tok->loc`/`tok->len` appear in `tok->hideset`, expansion is suppressed;
`expand_macro` returns `false`.

**subst** — `src/preprocess.c:970-1091`.
Processes the body token stream left-to-right:
- `#arg`: `find_arg` on `tok->next`; `stringize`; advance two (`# + param`)
  (`src/preprocess.c:976-983`).
- `tok ## ...` (with chained `A##B##C##D`): collect or copy LHS tokens, then
  loop pasting with RHS as long as the next token is `##`
  (`src/preprocess.c:986-1034`).  GNU `, ## __VA_ARGS__` handled at
  `src/preprocess.c:1006-1016`.
- Regular arg substitution: insert `arg->expanded` tokens; first token inherits
  `has_space` from the parameter reference in the body
  (`src/preprocess.c:1040-1050`).
- `__VA_OPT__`: include content if variadic arg non-empty, skip otherwise
  (`src/preprocess.c:1054-1082`).

### 2.2 Conditional inclusion

`#if` — `src/preprocess.c:1683-1688`.  `eval_const_expr`; `push_cond_incl`;
`skip_cond_incl` if false.

`#ifdef` / `#ifndef` — `src/preprocess.c:1691-1706`.  Direct `hashmap_get2`
(no macro expansion on the argument); `push_cond_incl`.

`#elif` — `src/preprocess.c:1709-1719`.  Errors if `ctx == IN_ELSE`.
Only evaluates `eval_const_expr` if not yet included; skips block otherwise.

`#else` — `src/preprocess.c:1722-1733`.  Errors if `ctx == IN_ELSE`; sets
`ctx = IN_ELSE`; skips if already included.

`#endif` — `src/preprocess.c:1736-1740`.  Pops `cond_incl`.

`skip_cond_incl` — `src/preprocess.c:1513-1528`.  Scans token-by-token;
when it sees a nested `#if*` it delegates to `skip_cond_incl2`
(`src/preprocess.c:1498-1511`) which skips to the matching `#endif`
recursively; stops at `#elif`/`#else`/`#endif` at current depth.

`eval_const_expr` — `src/preprocess.c:513-558`.  Pipeline:
1. `read_const_expr` (`src/preprocess.c:514`) — copy line, resolve
   `__has_*`/`defined`.
2. `preprocess2` (`src/preprocess.c:517`) — expand macros in the expression.
3. Post-expansion `defined(X)` pass (`src/preprocess.c:521-538`).
4. Replace remaining identifiers with `TK_PP_NUM "0"`
   (`src/preprocess.c:540-548`).
5. `convert_pp_tokens` (`src/preprocess.c:551`).
6. `const_expr` (`src/preprocess.c:554`).

`handle_pp_directive_in_arg` — `src/preprocess.c:837-886`.  Handles
`#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif` encountered inside a macro
argument list; shares the same `cond_incl` stack as the outer preprocessor.
Other directives: skip line.

### 2.3 #include resolution

`read_include_filename` — `src/preprocess.c:260-299`.  Three patterns:
1. `TK_STR`: extract `tok->str` (already decoded), set `*is_dquote = true`.
2. `<...>`: collect tokens between `<` and `>`, concatenate `loc`/`len` via
   `open_memstream`; respects `has_space` between tokens.
3. `TK_IDENT`: `preprocess2(copy_line(...))` then recurse.

`#include` — `src/preprocess.c:1619-1642`.  For `"..."`: try dirname of the
current file first (`dirname(start->file->name)`), then fall through to
`search_include_paths`.  For `<...>`: only `search_include_paths`.  Then
`include_file`.

`#include_next` — `src/preprocess.c:1645-1667`.  Finds the current file's
directory in `include_paths` (`strcmp` on `dirname`); calls
`search_include_next` from the following index.

`include_file` — `src/preprocess.c:1482-1495`.  `tokenize_file`; walk to
`TK_EOF`; overwrite that sentinel with the contents of `tok` (the token that
follows the `#include` line) to splice the included stream seamlessly.

`search_include_paths` — `src/preprocess.c:237-248`.  Absolute paths returned
immediately.  Otherwise `format("%s/%s", ...)` + `fopen` probe for each entry
in `include_paths`.

### 2.4 `##` and `#` operators

**`#` (stringize)** — `stringize` at `src/preprocess.c:784-814`.
Called from `subst` when `equal(tok, "#")` and `find_arg(args, tok->next)` is
non-NULL; otherwise `error_tok`.  Two `open_memstream` passes: first builds the
raw token text (spaces inserted where `has_space` is set on non-first tokens);
second wraps in quotes and escapes `\` and `"`.  Result is re-tokenized.
Position rule: `#` must be immediately followed by a parameter name
(`src/preprocess.c:977-979`).

**`##` (token paste)** — `paste` at `src/preprocess.c:816-823`.
Concatenates `lhs->loc[0..len)` and `rhs->loc[0..len)`, re-tokenizes; errors
if the result is not exactly one token.

**Chained paste `A##B##C##D`** — `subst` inner loop `src/preprocess.c:1003-1033`.
When the RHS is a macro argument and itself empty, the paste is skipped; when
the LHS is `,` and RHS is empty `__VA_ARGS__` (GNU extension), the comma is
deleted from the output; when RHS is non-empty, `paste(cur, rhs->tok)` runs and
remaining arg tokens are appended.  The loop continues as long as the next
token is another `##`.

### 2.5 Predefined macros (`init_macros`)

All `define_macro` / `add_macro` calls in `init_macros`
(`src/preprocess.c:1201-1475`).  None of these are gated on `-target elf`
in the swap-out branch — see §6 for the out-of-scope ELF entries.

**Standard C** (`src/preprocess.c:1203-1211`):
`__STDC__`=1, `__STDC_VERSION__`=201112L (C11), `__STDC_HOSTED__`=1,
`__STDC_NO_ATOMICS__`=1, `__STDC_NO_COMPLEX__`=1, `__STDC_NO_THREADS__`=1,
`__STDC_NO_VLA__`=1, `__STDC_UTF_16__`=1, `__STDC_UTF_32__`=1.

**Platform type/size/limit macros** (`src/preprocess.c:1213-1301`):
`__LP64__`, `__SIZEOF_*`, `__SIZE_TYPE__`, `__PTRDIFF_TYPE__`, `__WCHAR_TYPE__`,
`__WINT_TYPE__`, `__INT{8,16,32,64}_TYPE__`, `__UINT{8,16,32,64}_TYPE__`,
`__INTPTR_TYPE__`, `__UINTPTR_TYPE__`, `__INTMAX_TYPE__`, `__UINTMAX_TYPE__`,
`__CHAR_BIT__`, `__SCHAR_MAX__` (defined twice — see §8 note), `__SHRT_MAX__`,
`__INT_MAX__`, `__LONG_MAX__`, `__LONG_LONG_MAX__`, `__INT{8,16,32,64}_MAX__`,
`__UINT{8,16,32,64}_MAX__`, `__SIZE_MAX__`, `__INTMAX_MAX__`, `__UINTMAX_MAX__`,
`__PTRDIFF_MAX__`, `__INTPTR_MAX__`, `__UINTPTR_MAX__`,
`__FLT_*`, `__DBL_*`, `__LDBL_*` float constants, byte-order constants.

**ARM64 / AArch64** (`src/preprocess.c:1303-1308`):
`__aarch64__`=1, `__arm64__`=1, `__arm64`=1, `__AARCH64EL__`=1.

**Apple platform** (`src/preprocess.c:1309-1341`):
`__APPLE__`=1, `__MACH__`=1, `__DARWIN_C_LEVEL`=900000L,
`TARGET_OS_MAC`=1, `TARGET_OS_OSX`=1, `TARGET_OS_{IPHONE,IOS,WATCH,TV,...}`=0,
`TARGET_CPU_ARM64`=1, `TARGET_CPU_{ARM,X86,X86_64}`=0,
`TARGET_RT_{LITTLE_ENDIAN,64_BIT,MAC_MACHO}`=1,
`__MAC_OS_X_VERSION_{MIN_REQUIRED,MAX_ALLOWED}`=140000 (macOS 14 baseline),
`__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__`=140000,
`__ENVIRONMENT_OS_VERSION_MIN_REQUIRED__`=140000.

**GCC compatibility** (`src/preprocess.c:1343-1390`):
`__GNUC__`=12, `__GNUC_MINOR__`=1, `__GNUC_PATCHLEVEL__`=0,
`__GNUC_STDC_INLINE__`=1, `__VERSION__`="ncc 1.0 compatible",
keyword aliases (`__const__`→const, `__inline__`→inline, `__volatile__`→volatile,
`__attribute`→`__attribute__`, `__signed__`→signed, `__restrict__`→restrict,
`__extension__`→"", `__complex`→`_Complex`, `__real`→`__real__`,
`__imag`→`__imag__`, `__asm`→asm), type aliases
(`__uint128_t`→`unsigned long`, `__int128_t`/`__int128`→long,
`_Float16`→float, `__builtin_va_list`→`void *`),
`__alignof__`→`_Alignof`.

**GCC Atomics** (`src/preprocess.c:1350-1366`):
`__ATOMIC_{RELAXED,CONSUME,ACQUIRE,RELEASE,ACQ_REL,SEQ_CST}` order constants,
`__atomic_load_n`/`store_n`/`exchange_n`/`compare_exchange_n` as
single-threaded stubs,
`__GCC_HAVE_SYNC_COMPARE_AND_SWAP_{1,2,4,8}`=1.

**Builtin function-like macros** (`src/preprocess.c:1391-1440`):
`__builtin_expect(x,y)`→`(x)`, fabs/fabs/fabsl, inf/nan variants,
`__builtin_huge_val{f,}()`, `__builtin_offsetof`, `__builtin_unreachable()`,
`__builtin_assume()`, `__builtin_trap()`→`abort()`,
memory/string builtins (`__builtin_memset`→`memset`, etc.),
`__builtin_printf`→`printf`, signbit variants.
Note: `__builtin_types_compatible_p`, `__builtin_constant_p`, and bit-manipulation
builtins (`__builtin_clz*`, etc.) are **not** defined here; they are handled
directly in `parse.c` primary() (`src/preprocess.c:1404-1410`).

**Date/time** (`src/preprocess.c:1444-1456`):
`__DATE__` and `__TIME__` computed once at `init_macros()` call time via
`time(NULL)` / `localtime`.

**No-op** (`src/preprocess.c:1460`):
`_Pragma(x)`→"" (ncc ignores all pragmas).

**Handler macros** (`src/preprocess.c:1463-1475`):
`__FILE__` (→`file_macro`), `__LINE__` (→`line_macro`),
`__COUNTER__` (→`counter_macro`), `__TIMESTAMP__` (→`timestamp_macro`),
`__BASE_FILE__` (→`base_file_macro`); registered via `add_macro` with
`m->handler` set, not via `define_macro`.

### 2.6 `#line` directive

`src/preprocess.c:1744-1752`.
`preprocess2(copy_line(...))` + `convert_pp_tokens` → validate `TK_NUM` of
`TY_INT`.  Sets `start->file->line_delta = t->val - start->line_no`.
If the next token is `TK_STR`, sets `start->file->display_name = t->next->str`.
The `line_delta` is copied into each output token's `tok->line_delta` field
inside the main preprocess2 loop at `src/preprocess.c:1610`.

### 2.7 Hideset

Five operations, all in `src/preprocess.c:111-155`:

- `new_hideset(char *name)` — alloc one node.
- `hideset_union(hs1, hs2)` — concatenation; no deduplication (duplicates are
  harmless because `hideset_contains` terminates on first match).
- `hideset_intersection(hs1, hs2)` — elements of hs1 whose name appears in hs2;
  called for the function-like macro case to intersect `macro_tok->hideset` and
  `rparen->hideset`.
- `hideset_contains(hs, s, len)` — linear `strncmp` scan.
- `add_hideset(tok, hs)` — copy every token, union `hs` into each copy.

The `hideset` field on every token produced by `tokenize` is `NULL` (calloc'd).
The preprocessor stamps hidesets only on tokens it copies; the originals from
the tokenizer are never modified.

### 2.8 Position-flag (`at_bol`, `has_space`) inheritance

Flags are set precisely by the tokenizer; the preprocessor propagates them
through copies:

- `copy_token` copies all fields, so `at_bol` and `has_space` follow the source
  token by default.
- In `subst` (`src/preprocess.c:1040-1046`): the **first token** of a
  substituted (pre-expanded) argument inherits `has_space` from the **parameter
  reference** in the macro body — i.e., the spacing visible at the call site
  governs how the expansion looks in the output stream.
- `read_macro_definition` (`src/preprocess.c:1546`): uses `!tok->has_space &&
  equal(tok, "(")` to distinguish function-like (`FOO(x)`) from object-like
  (`FOO (x)` — the second is object-like with body starting at `(`).
- In `read_include_filename` (<...> path, `src/preprocess.c:285`): `t->has_space`
  is read to decide whether to insert a space in the reconstructed filename.
- `at_bol` is used by `is_hash` to recognize directive lines; it is not modified
  by the preprocessor itself (except implicitly via re-tokenization inside
  `stringize` and `paste`).

**Revert note**: commit `ad8f346` on swap-out applied the `b710056` preprocess
hunk, reverting `paste()` and the two `expand_macro()` blocks to match `main`.
The reverted code had modified `at_bol`/`has_space` on expanded tokens; the
current code does not.  The spec should describe the post-revert behavior (which
is `main`'s behavior).

---

## 3. Token field usage

Fields are listed per the `Token` struct in `src/cc.h:91-108`.

Tokenizer initialization references `src/tokenize.c` on the swap-out branch,
which is the Phase-1 spec-derived implementation (`src/tokenize.c` after Phase 1
swap-in, authored as `tokenize_v2.c`).

| Field | Tokenizer init | `preprocess.c` read | `preprocess.c` write | Notes |
|---|---|---|---|---|
| `kind` | `new_token` sets to the passed `TokenKind` | Extensively: `is_hash`, `append`, `find_macro`, `expand_macro`, `skip_cond_incl*`, `eval_const_expr`, `primary`, `read_include_filename`, `#line` handler, all directive checks | `new_eof` writes `TK_EOF`; `read_const_expr` and `eval_const_expr` write `TK_PP_NUM` for `defined()`/`__has_*` results; `convert_pp_tokens` (via `preprocess`) writes `TK_NUM` | `find_macro` accepts both `TK_IDENT` and `TK_KEYWORD` — keywords can be macro names |
| `next` | `NULL` (calloc) | All list traversals | All list-building helpers (`append`, `add_hideset`, `subst`, etc.) | — |
| `loc` | Points into `file->contents` | `find_macro`, `hideset_contains`, `paste`, `stringize`, all `equal` calls, `read_include_filename` | `copy_token` inherits; `new_eof` inherits; `read_const_expr` line 415/466 writes `"1"`/`"0"` | The `"0"`/`"1"` overwrites point at static string literals, not into source buffers |
| `len` | Byte span of source spelling | `find_macro`, `hideset_contains`, `paste`, `primary` (PP_NUM) | `new_eof` writes `0`; `read_const_expr` line 416/467 writes `1`; `copy_token` inherits | — |
| `val` | Set for `TK_NUM` integers and char literals | `primary` line 762 (`TK_NUM` case) | `copy_token` inherits; `convert_pp_tokens` writes | Preprocessor's `primary()` only reads `val` for `TK_NUM` (char literals); `TK_PP_NUM` parses via `strtol` from `loc` |
| `fval` | Set for `TK_NUM` floats | Not directly read by preprocessor logic | `copy_token` inherits; `convert_pp_tokens` writes | float values are not used in `#if` constant expressions |
| `ty` | Set for `TK_NUM` and `TK_STR` | `read_include_filename` reads `tok->ty->array_len`; `__has_include` reads `arg->ty->array_len`; `#line` handler checks `t->ty->kind == TY_INT` | `copy_token` inherits; `convert_pp_tokens` writes | `ty` must be non-NULL on `TK_STR` tokens before `read_include_filename` runs |
| `str` | Decoded string contents for `TK_STR` | `read_include_filename` Pattern 1: `strndup(tok->str, tok->ty->array_len-1)` | `copy_token` inherits | — |
| `file` | Set by `new_token` from `current_file` | `file_macro`, `line_macro`, `counter_macro`, `timestamp_macro`, `base_file_macro`; `include_file` (dirname of current file); `#line` handler | `copy_token` inherits | `file->line_delta` and `file->display_name` written by `#line` handler |
| `line_no` | Set by `add_line_numbers` pass | `line_macro` (`tmpl->line_no + tmpl->file->line_delta`); `#line` handler (`start->line_no`) | `copy_token` inherits | — |
| `line_delta` | `0` (calloc) — not set by tokenizer | Not read by preprocessor logic (the per-token copy is written before any downstream read) | `preprocess2` main loop line 1610 writes `tok->line_delta = tok->file->line_delta` on every non-directive output token | This field is a per-token snapshot of the file's current `line_delta` at the moment the token was emitted from the preprocessor; it is not set by the tokenizer |
| `at_bol` | Set in main tokenizer loop | `is_hash`, `skip_line`, `copy_line`, `read_include_filename` (<...>), `read_macro_args`, `skip_cond_incl*` | `copy_token` inherits | Preprocessor **relies** on tokenizer setting this correctly; never sets it independently (except inside `stringize`/`paste` which re-tokenize) |
| `has_space` | Set in main tokenizer loop | `read_include_filename` (<...> `src/preprocess.c:285`); `read_macro_definition` line 1546 (function-like detection); `subst` line 1045 | `subst` line 1045 writes `cur->has_space = tok->has_space` for first expanded arg token; `copy_token` inherits | Critical for function-like detection: `FOO(x)` vs `FOO (x)` |
| `hideset` | `NULL` (calloc) | `expand_macro` line 1105 (`hideset_contains`); `add_hideset` line 151 (`hideset_union`) | `add_hideset` (via `copy_token` then union); `expand_macro` stamps via `add_hideset` | **Relies on `NULL` initial value** — `add_hideset` calls `hideset_union(t->hideset, hs)` where NULL is valid input |
| `origin` | `NULL` (calloc) | `file_macro`/`line_macro` (origin chain walk); `error_tok` (macro use site walk) | `expand_macro` lines 1124-1125 and 1149-1150: `t->origin = tok` / `t->origin = macro_tok` | **Relies on `NULL` initial value** — origin chain walk terminates on NULL |

---

## 4. Edge cases observed

Grep results for flagged comments in `src/preprocess.c`:

| Line | Keyword | Summary |
|---|---|---|
| 833 | (function comment) | `handle_pp_directive_in_arg` — handles `#if*` inside macro arg lists for the Linux `decompress_inflate.c` pattern (ternary split across conditional compilation). |
| 1007 | `GNU extension` | `, ## __VA_ARGS__` with empty args deletes the preceding comma — GCC extension, not C standard. |
| 1131 | `must` | Function-like macro `must` be followed by `(` — if not, treat the identifier as non-macro (returns false, no expansion). |
| 1343 | `GCC compatibility` | Advertise `__GNUC__`=12; comment explains this is to open modern GCC feature paths in kernel headers. |
| 1350 | `GCC atomic` | Atomic memory order constants (`__ATOMIC_RELAXED` etc.) are GCC-compat constants, not C11 `_Atomic` support. |
| 1358 | `GCC atomic builtins` | `__atomic_*` builtins implemented as single-threaded stubs via macro expansion to `__sync_*` — which are themselves unimplemented (they will link-error unless workaround applied). |
| 1404 | (inline comment) | `__builtin_types_compatible_p` and `__builtin_constant_p` handled in `parse.c`, NOT here. |
| 1409 | (inline comment) | Bit-manipulation builtins (`__builtin_clz*` etc.) NOT defined as macros; parsed directly in `parse.c`. |
| 1428–1435 | `tricky` | `__builtin_printf` is tricky on ARM64 because it's variadic; comment explains why it is mapped to `printf` directly rather than treated as a builtin with special ABI handling. |

---

## 5. GCC/clang extensions used in `preprocess.c` source

Per the project's pure-C constraint, the Phase 2 reimplementation must use
C11 only.  The following non-standard or POSIX-only constructs appear in
`src/preprocess.c` itself (not in string arguments to `define_macro`):

| Lines | Extension | C11 replacement |
|---|---|---|
| 118, 136, 146, 162, 304, 472, 894, 909, 943, 971, 1095, 1550, 1599 | `= {}` empty struct/union initializer — GCC/clang extension; C11 requires at least one initializer (`{0}` is the portable form) | Replace with `= {0}` |
| 282, 788, 800 (+ `read_file` in tokenize.c) | `open_memstream` — POSIX.1-2008, not in C11 | Replace with `calloc`+`realloc` grow-buffer or `snprintf` pattern |
| 3 | `#include <libgen.h>` — POSIX; `dirname()` used at lines 1627, 1652 | Use manual `strrchr`-based dirname extraction |
| Throughout | `strndup` / `strdup` — POSIX.1-2001/2008, not C11 | Implement as wrappers over `malloc`+`memcpy` / `strlen`+`malloc`+`strcpy` |
| 188 | `strndup_checked` — project wrapper around `strndup` (POSIX) | Same as above |

No `__attribute__`, `__builtin_*` (in source logic), `typeof`, nested functions,
statement expressions, labels-as-values, or `__thread` are used in
`preprocess.c`'s own source code.

The `static const char *` arrays inside function scope
(`src/preprocess.c:354-364`, `379-394`, `1448`) are standard C11.

---

## 6. macOS predefine set (Phase 2 scope)

All `define_macro()` / `add_macro()` calls in `init_macros()`, organized by
category.  Phase 2 scope is **macOS-only**; the `-target elf` predefined-macro
additions from `main` commits `4ed0320` and `8fe8dda` are **not present** in
swap-out's `preprocess.c` (confirmed by grep — no `__ARM_ARCH`, `__ELF__`,
`__ARM_ARCH_8A__`, or `__ARM_PCS_AAPCS64` in `src/preprocess.c` on this branch).

| Category | Macros | Lines | Phase 2 scope? |
|---|---|---|---|
| **Standard C** | `__STDC__`, `__STDC_VERSION__` (201112L), `__STDC_HOSTED__`, `__STDC_NO_ATOMICS__`, `__STDC_NO_COMPLEX__`, `__STDC_NO_THREADS__`, `__STDC_NO_VLA__`, `__STDC_UTF_16__`, `__STDC_UTF_32__` | 1203–1211 | **In scope** |
| **Platform sizes/types** | `__LP64__`, `__SIZEOF_*`, `__SIZE_TYPE__`, `__PTRDIFF_TYPE__`, `__WCHAR_TYPE__`, `__WINT_TYPE__`, `__INT*_TYPE__`, `__UINT*_TYPE__`, `__INTPTR_TYPE__`, `__UINTPTR_TYPE__`, `__INTMAX_TYPE__`, `__UINTMAX_TYPE__` | 1214–1242 | **In scope** |
| **Limits** | `__CHAR_BIT__`, `__SCHAR_MAX__` (**defined twice**, lines 1245 + 1296), `__SHRT_MAX__`, `__INT_MAX__`, `__LONG_MAX__`, `__LONG_LONG_MAX__`, `__INT*_MAX__`, `__UINT*_MAX__`, `__SIZE_MAX__`, `__INTMAX_MAX__`, `__UINTMAX_MAX__`, `__PTRDIFF_MAX__`, `__INTPTR_MAX__`, `__UINTPTR_MAX__` | 1244–1263 | **In scope** |
| **Float constants** | `__FLT_*`, `__DBL_*`, `__LDBL_*` (min, max, epsilon, denorm, infinity/nan flags, mantissa digits, decimal exponent range), `__FINITE_MATH_ONLY__` | 1265–1295 | **In scope** |
| **Byte order** | `__ORDER_LITTLE_ENDIAN__`=1234, `__ORDER_BIG_ENDIAN__`=4321, `__BYTE_ORDER__`=1234, `__LITTLE_ENDIAN__`=1 | 1298–1301 | **In scope** |
| **ARM64/AArch64 arch** | `__aarch64__`=1, `__arm64__`=1, `__arm64`=1, `__AARCH64EL__`=1 | 1303–1307 | **In scope** |
| **Apple platform** | `__APPLE__`=1, `__MACH__`=1, `__DARWIN_C_LEVEL`=900000L | 1308–1310 | **In scope** |
| **Apple target conditionals** | `TARGET_OS_MAC`=1, `TARGET_OS_OSX`=1, `TARGET_OS_{IPHONE,IOS,WATCH,TV,SIMULATOR,EMBEDDED}`=0, `TARGET_CPU_ARM64`=1, `TARGET_CPU_{ARM,X86,X86_64}`=0, `TARGET_RT_LITTLE_ENDIAN`=1, `TARGET_RT_BIG_ENDIAN`=0, `TARGET_RT_64_BIT`=1, `TARGET_RT_MAC_MACHO`=1, `TARGET_OS_MACCATALYST`=0, `TARGET_OS_DRIVERKIT`=0 | 1313–1329 | **In scope** |
| **Darwin deployment target** | `__MAC_OS_X_VERSION_MIN_REQUIRED`=140000, `__MAC_OS_X_VERSION_MAX_ALLOWED`=140000, `__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__`=140000, `__ENVIRONMENT_OS_VERSION_MIN_REQUIRED__`=140000 | 1338–1341 | **In scope** |
| **GCC compatibility** | `__GNUC__`=12, `__GNUC_MINOR__`=1, `__GNUC_PATCHLEVEL__`=0, `__GNUC_STDC_INLINE__`=1, `__VERSION__`="ncc 1.0 compatible" | 1344–1348 | **In scope** |
| **GCC atomic order constants** | `__ATOMIC_RELAXED`=0 … `__ATOMIC_SEQ_CST`=5 | 1351–1356 | **In scope** |
| **GCC atomic builtins (stubs)** | `__atomic_load_n`, `__atomic_store_n`, `__atomic_exchange_n`, `__atomic_compare_exchange_n`, `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_{1,2,4,8}` | 1359–1366 | **In scope** |
| **GCC keyword aliases** | `__alignof__`→`_Alignof`, `__const__`/`__const`→const, `__inline__`/`__inline`→inline, `__uint128_t`/`__int128_t`/`__int128`/`_Float16`, `__volatile__`/`__volatile`→volatile, `__attribute`→`__attribute__`, `__signed__`/`__signed`→signed, `__restrict__`/`__restrict`→restrict, `__extension__`→"", `__complex`→`_Complex`, `__real`→`__real__`, `__imag`→`__imag__`, `__asm`→asm, `__builtin_va_list`→`void *` | 1368–1389 | **In scope** |
| **Builtin function-like macros** | `__builtin_expect`, fabs/inf/nan/huge_val variants, `__builtin_offsetof`, `__builtin_unreachable`, `__builtin_assume`, `__builtin_trap`, memory/string builtins, `__builtin_printf`/sprintf/putchar/puts, signbit variants, `_Pragma(x)` | 1391–1460 | **In scope** |
| **Date/time** | `__DATE__`, `__TIME__` (computed at init time) | 1444–1456 | **In scope** |
| **Handler macros** | `__FILE__`, `__LINE__`, `__COUNTER__`, `__TIMESTAMP__`, `__BASE_FILE__` | 1463–1475 | **In scope** |
| **ELF/Linux predefines** | `__ARM_ARCH`=8, `__ARM_ARCH_8A__`, `__ARM_PCS_AAPCS64`, `__ELF__`, Linux include-path setup | NOT PRESENT on swap-out (added on main in `4ed0320`/`8fe8dda`) | **OUT OF SCOPE** — Phase 5 work |

The out-of-scope ELF entries were explicitly deferred in the divergence log
entry for `4ed0320` (swap-out-log.md): "Recommend deferring — the simpler
initial spec covers macOS only; `-target elf` becomes a separate spec extension
when xv6/NetBSD/Linux work resumes."  Phase 2 must not define `__ARM_ARCH`,
`__ARM_ARCH_8A__`, or `__ARM_PCS_AAPCS64` in its init-macros implementation.

---

## Notes for the spec author

Gotchas, non-obvious invariants, and places where a test case would be valuable:

- **`__SCHAR_MAX__` defined twice.** `init_macros` calls `define_macro("__SCHAR_MAX__", "127")` at line 1245 and again at line 1296. The second call silently overwrites the first via `hashmap_put`. This is a latent bug (same value, so no observable difference today), but the spec should define the de-duplicated list and the reimplementation should not perpetuate the duplicate.

- **`find_macro` accepts `TK_KEYWORD`.** Macro lookup works on both `TK_IDENT` and `TK_KEYWORD` (`src/preprocess.c:185`).  A definition like `#define inline __forceinline` is legal C; the spec must not restrict lookup to `TK_IDENT` only.

- **Function-like detection is `has_space`-sensitive.** `read_macro_definition` at line 1546 uses `!tok->has_space && equal(tok, "(")`.  If the tokenizer ever sets `has_space` incorrectly on a `(` that immediately follows a macro name, the macro will be misregistered as object-like.  This is the only place the preprocessor uses `has_space` to make a structural decision (vs. propagation).

- **`subst` uses `expanded`, not `tok`.**  Regular argument substitution (`src/preprocess.c:1041`) inserts `arg->expanded` (pre-expanded), never `arg->tok` (raw).  Stringize (`src/preprocess.c:981`) uses `arg->tok` (raw), not `arg->expanded`.  This is the C standard's "rescan before substitution, but not before stringization" rule; it must be explicit in the spec.

- **`hideset_union` does not deduplicate.**  Duplicate entries are harmless (contains terminates on first match) but will accumulate on deeply-nested macro expansions.  A reimplementation that deduplicates is also correct.

- **Hideset intersection is used only for function-like macros.** The intersection of `macro_tok->hideset` and `rparen->hideset` (`src/preprocess.c:1144`) is the C standard's "painter's rule" for function-like macros.  Object-like macros use `union(tok->hideset, {m->name})` directly.  This asymmetry must be preserved.

- **`origin` is set only on body tokens, not on the appended `tok->next` tail.** Lines 1124-1125 and 1149-1150 walk only the body up to `TK_EOF`; the `append`ed rest-of-stream tokens are not touched.  `file_macro`/`line_macro` walk the chain, so they will correctly land on the body token when origin is set.

- **`include_file` splices by mutating the included file's `TK_EOF`.** Line 1492-1493 overwrites the included file's terminal `TK_EOF` node's fields with the continuing token.  This means the `TK_EOF` node is reused as the first continuing token — its `kind` changes from `TK_EOF` to whatever `tok->kind` is.  If the caller holds a reference to the original `TK_EOF` for any reason, it will see a mutated token.

- **`#pragma once` is a stub.** `src/preprocess.c:1758-1761` marks it but does not track which files have been included; re-including a file with `#pragma once` will include it again.  The spec must decide whether to implement this or continue the stub.

- **`__has_attribute` and `__has_builtin` operate on a hardcoded list.** The list of supported attributes (`src/preprocess.c:354-364`) and builtins (`src/preprocess.c:379-394`) is embedded in `read_const_expr` and will go stale as new attributes/builtins are added.  The spec should note this is an explicit allowlist.

- **`__has_feature` always returns 0.** `src/preprocess.c:408-409`.  This is intentional but will cause headers that gate on `__has_feature(cxx_rvalue_references)` etc. to take the `#else` path.  For macOS SDK headers this is almost always the right path for C code, but the spec should call it out.

- **`eval_const_expr` calls `preprocess2` recursively.** This means `#if defined(X)` where `X` expands to an expression is handled, but it also means macros in `#if` conditions are fully expanded — including object-like macros that expand to identifiers, which are then replaced with 0 at line 540-548.  Test case: `#define ZERO 0\n#if ZERO\n` should not include the block.

- **`handle_pp_directive_in_arg` shares `cond_incl`.** The in-arg directive handler pushes onto the same `cond_incl` stack used by the main loop.  A `#if` inside a macro argument that is never closed would leave a dangling entry, caught only by `preprocess`'s post-loop check.  Test case: a macro argument containing `#ifdef X\n...` without `#endif`.

- **`line_delta` is a per-token copy, not looked up on demand.** Every output token gets `tok->line_delta = tok->file->line_delta` written at `src/preprocess.c:1610`.  Tokens inside macro expansions (which do not pass through that branch of `preprocess2`) retain whatever `line_delta` they inherited from `copy_token` or from the body tokens.  The spec should clarify what `line_delta` means on an expanded body token.

- **`stringize` does not handle `has_space` on the first arg token.** The leading token gets no leading space (`t != arg` guard at `src/preprocess.c:793`), even if the first token itself has `has_space == true`.  This matches GCC behavior, but it's subtle: the stringization of ` x` (space before x) and `x` produce the same string.

- **`timestamp_macro` always returns `"Unknown"`.** A real implementation would stat the source file and format its modification time.  The stub may cause `__TIMESTAMP__`-gated guards to mismatch between ncc and clang; the spec should document this as a known simplification.

- **`__DATE__` / `__TIME__` are fixed at `init_macros()` call time**, not per-TU.  If the compiler processes multiple TUs in a single invocation (future driver work), they will all get the same date/time.  Currently a non-issue; worth noting.

- **`fopen` leaks in `search_include_paths`.** Line 244: `if (fopen(path, "r"))` — the returned `FILE *` is not closed.  This is a resource leak (one file descriptor per include probe).  The spec should use `access()` or `stat()` for path probing to avoid this.

- **`paste` with an empty argument token list is handled in `subst`, not `paste` itself.** When the LHS or RHS of `##` is an empty-expansion arg, `subst` skips the paste at `src/preprocess.c:989-991`; `paste` itself would error on an empty string.  The spec must describe the empty-arg guard before calling paste.
