# ncc Parser — Declarations Sub-Chunk (Phase 4, 04a)

This sub-chunk specifies the **declaration zone** of `src/parse.c`:
declaration specifiers, declarators, struct/union/enum bodies,
array and VLA dimensioning, attributes, and the top-level entry
point that dispatches between declarations and function
definitions.

Cross-cutting decisions (typedef rules, `is_typename`, scope
mechanics, attribute disposition table) live in `04_parse.md`;
this file cites them.

**Coverage in `src/parse.c`:**

| Lines | Section |
|---|---|
| 6–113 | Scope structures (also referenced by `04c_stmt.md`) |
| 403–462 | Variable creation helpers |
| 464–614 | `VarAttr` type + VLA helpers |
| 681–957 | `declspec` (declaration specifiers) |
| 964–1071 | `pointers`, `func_params` |
| 1074–1146 | `array_dimensions`, `type_suffix` |
| 1150–1252 | `declarator`, `abstract_declarator`, `typename_` |
| 1258–1411 | `struct_members`, `struct_layout`, `union_layout` |
| 1414–1428 | `skip_asm_label` |
| 1431–1582 | `attribute_list` |
| 1584–1690 | `struct_decl`, `union_decl` |
| 1696–1758 | `enum_specifier` |
| 4548–4648 | `declaration` (local declarations) |
| 5552–5660 | `parse` (top-level entry) |
| 5662–end | `function`, `global_variable`, `parse_typedef` (the dispatch targets) |

When this document and `main`'s observable behavior disagree, this
document is wrong and must be updated, unless the divergence is
recorded in §13 of `04_parse.md`.

---

## A. Declaration grammar overview

The declaration zone implements the C11 declaration grammar plus
GCC extensions ncc accepts.  The grammar is recursive descent;
each production maps to one or two functions.

```
translation-unit       := { external-declaration }
external-declaration   := function-definition | declaration
declaration            := declspec [ init-declarator { "," init-declarator } ] ";"
                       |  declspec ";"                    // bare struct/union/enum decl
                       |  "_Static_assert" "(" const-expr [ "," string ] ")" ";"
init-declarator        := declarator [ asm-label ] [ attribute-list ] [ "=" initializer ]
declspec               := { storage-class | type-spec | qualifier | function-spec
                          | alignment-spec | attribute | "__extension__" }+
declarator             := pointers direct-declarator
direct-declarator      := identifier
                       |  "(" declarator ")"
                       |  direct-declarator type-suffix
type-suffix            := "(" func-params ")"
                       |  "[" array-dimensions "]"
                       |  ε
abstract-declarator    := pointers [ "(" abstract-declarator ")" ] [ type-suffix ]
typename               := declspec abstract-declarator
struct-decl            := ( "struct" | "union" ) [ identifier ] [ attribute-list ]
                          [ "{" struct-members "}" ] [ attribute-list ]
enum-specifier         := "enum" [ identifier ] [ ":" typename ]
                          [ "{" enum-list [ "," ] "}" ]
function-definition    := declspec declarator [ kr-decls ] compound-stmt
```

The key dispatch points:
- **`is_typename(tok)`** — at every position where a declaration
  could begin (statement context, struct member context, parameter
  context, top-level), `is_typename` decides the branch.  See
  `04_parse.md` §5.
- **Function definition vs declaration** — at top level, after
  parsing `declspec` + `declarator`, the next token decides:
  `{` → function definition; `;` / `,` → declaration; type-keyword
  → K&R-style function definition (§E.2).
- **Nested declarator disambiguation** — `declarator` peeks ahead
  to distinguish `int (*p)(int)` (function pointer) from
  `int (x)` (parenthesized identifier).  See §C.4.

Function names (the four major recursive entry points):
`declspec`, `declarator`, `abstract_declarator`, `typename_`.
Plus `struct_decl`, `union_decl`, `enum_specifier` for tag types.
The reimplementation may rename internals; the four-function
public surface from `04_parse.md` §2 is the durable contract.

---

## B. `declspec` — declaration specifiers

`declspec(Token **rest, Token *tok, VarAttr *attr)` returns a
`Type *` and (via `attr`) populates a `VarAttr` storage-class
record.  The function is the heart of declaration parsing: it
processes any sequence of type-specifiers, storage-class
specifiers, qualifiers, function specifiers, alignment specifiers,
and attribute lists in any order, combining them into a single
canonical `Type *` plus `VarAttr`.

### B.1 The state machine

Internally, `declspec` accumulates a bitmask `counter` that
records which primitive type specifiers have been seen.  Each
recognized primitive contributes a unique increment:

| Specifier | Increment |
|---|---|
| `void` | `VOID = 1<<0` |
| `_Bool` | `BOOL = 1<<2` |
| `char` | `CHAR = 1<<4` |
| `short` | `SHORT = 1<<6` |
| `int` | `INT = 1<<8` |
| `long` | `LONG = 1<<10` (additive — second `long` produces `2<<10`) |
| `float` | `FLOAT = 1<<12` |
| `double` | `DOUBLE = 1<<14` |
| `OTHER` (struct/union/enum/typedef/typeof/`__builtin_va_list`) | `1<<16` |
| `signed` | `SIGNED = 1<<17` (set as flag) |
| `unsigned` | `UNSIGNED = 1<<18` (set as flag) |
| `_Complex` / `__complex__` | `COMPLEX = 1<<19` (set as flag) |

After each token consumption, the resulting `counter` value
selects the canonical type via a switch table.  The shifts are
chosen so duplicate `int` (`INT + INT`) overflows into invalid
counter space and triggers the `default → error_tok("invalid
type")` arm — duplicate primitive specifiers are rejected.

Two `long`s produce `LONG + LONG = 2<<10`, which is a valid
counter value selecting `long long` (or `unsigned long long`,
etc.).  Three `long`s overflow further and are rejected.

### B.2 The canonical-type switch

The switch table below is the normative mapping from
`counter` value to predefined `Type *` singleton (or constructed
`_Complex` type).  Order matters in the source for compactness;
order does not matter normatively.  Any combination not in the
table is an error.

| Counter value | Type |
|---|---|
| `VOID` | `ty_void` |
| `BOOL` | `ty_bool` |
| `CHAR`, `SIGNED+CHAR` | `ty_char` |
| `UNSIGNED+CHAR` | `ty_uchar` |
| `SHORT`, `SHORT+INT`, `SIGNED+SHORT`, `SIGNED+SHORT+INT` | `ty_short` |
| `UNSIGNED+SHORT`, `UNSIGNED+SHORT+INT` | `ty_ushort` |
| `INT`, `SIGNED`, `SIGNED+INT` | `ty_int` |
| `UNSIGNED`, `UNSIGNED+INT` | `ty_uint` |
| `LONG`, `LONG+INT`, `SIGNED+LONG`, `SIGNED+LONG+INT` | `ty_long` |
| `UNSIGNED+LONG`, `UNSIGNED+LONG+INT` | `ty_ulong` |
| `LONG+LONG`, `LONG+LONG+INT`, `SIGNED+LONG+LONG`, `SIGNED+LONG+LONG+INT` | `ty_longlong` |
| `UNSIGNED+LONG+LONG`, `UNSIGNED+LONG+LONG+INT` | `ty_ulonglong` |
| `FLOAT` | `ty_float` |
| `DOUBLE` | `ty_double` |
| `LONG+DOUBLE` | `ty_ldouble` |
| `COMPLEX`, `COMPLEX+DOUBLE` | `complex_type(ty_double)` |
| `COMPLEX+FLOAT` | `complex_type(ty_float)` |
| `COMPLEX+LONG+DOUBLE` | `complex_type(ty_ldouble)` |
| `COMPLEX+INT`, `COMPLEX+SIGNED`, `COMPLEX+SIGNED+INT` | `complex_type(ty_int)` |
| `COMPLEX+UNSIGNED`, `COMPLEX+UNSIGNED+INT` | `complex_type(ty_uint)` |
| `COMPLEX+LONG`, `COMPLEX+LONG+INT`, `COMPLEX+SIGNED+LONG`, `COMPLEX+SIGNED+LONG+INT` | `complex_type(ty_long)` |
| `COMPLEX+UNSIGNED+LONG`, `COMPLEX+UNSIGNED+LONG+INT` | `complex_type(ty_ulong)` |
| `COMPLEX+LONG+LONG` and the four `+INT`/`+SIGNED` variants | `complex_type(ty_longlong)` |
| `COMPLEX+UNSIGNED+LONG+LONG`, `COMPLEX+UNSIGNED+LONG+LONG+INT` | `complex_type(ty_ulonglong)` |
| `COMPLEX+SHORT`, `+INT`/`+SIGNED` variants | `complex_type(ty_short)` |
| `COMPLEX+UNSIGNED+SHORT`, `+INT` | `complex_type(ty_ushort)` |
| `COMPLEX+CHAR`, `COMPLEX+SIGNED+CHAR` | `complex_type(ty_char)` |
| `COMPLEX+UNSIGNED+CHAR` | `complex_type(ty_uchar)` |
| anything else | error |

Default starting type before any specifier is seen: `ty_int`.
This is the C "default int" rule for old-style declarations
(K&R); modern declarations always explicitly include a type
specifier before the loop terminates, which causes `counter` to
become non-zero.

### B.3 Storage class specifiers

Recognized within the loop and recorded on `attr`:

| Keyword | `VarAttr` field |
|---|---|
| `typedef` | `attr->is_typedef = true` |
| `static` | `attr->is_static = true` |
| `extern` | `attr->is_extern = true` |
| `inline` | `attr->is_inline = true` |
| `_Noreturn` | `attr->is_noreturn = true` |
| `_Thread_local`, `__thread` | `attr->is_tls = true` |
| `register` | (consumed, no flag) |
| `auto` | (not in `is_typename`; if it appears at top-of-loop, the loop body's storage-class branch covers it as no-op via the keyword test) |

C11 prohibits combining `typedef` with another storage class;
ncc currently does not enforce this, matching `main`'s permissive
behavior.

If `attr` is NULL, storage-class keywords are still consumed
without effect — this lets `typename_` (which passes NULL `attr`)
tolerate stray storage-class keywords without crashing.

### B.4 Type qualifiers

`const`, `volatile`, `restrict`, `__restrict`, `__restrict__` are
recognized inside the loop as boolean accumulators:

```
is_const     = is_const    || equal(tok, "const")
is_volatile  = is_volatile || equal(tok, "volatile")
```

`restrict` and the GCC `__restrict` / `__restrict__` forms are
consumed but produce no flag — ncc does not enforce restrict
semantics.

After the main loop terminates, if any of `is_atomic`,
`is_const`, `is_volatile` is set, the resulting `Type *` is
`copy_type`'d (to avoid mutating shared singletons) and the
qualifier flag is set on the copy.

### B.5 `_Atomic`

Two forms:
- `_Atomic` as a qualifier — sets `is_atomic`, applied at end-of-
  loop to the copied type.
- `_Atomic ( type-name )` as a type specifier — replaces the
  current `ty` with the parsed type-name and sets `is_atomic`.

The parenthesized form takes precedence: when `_Atomic` is
followed by `(`, the parser recurses via `typename_`.

### B.6 `_Alignas`

`_Alignas ( N )` — `N` is a constant expression evaluated via
`const_expr_val`; sets `attr->align`.

`_Alignas ( type-name )` — parsed via `typename_`; the type's
alignment is copied to `attr->align`.

If `attr` is NULL (e.g., inside `typename_`), `_Alignas` is an
error: it cannot appear in a context that has no `VarAttr`
target.  Matching `main`'s `error_tok(tok, "_Alignas is not
allowed here")`.

### B.7 `typeof` / `__typeof` / `__typeof__`

Three syntactic forms:
- `typeof ( type-name )` — recursively parses a type-name.
- `typeof ( expression )` — parses an expression, runs `add_type`,
  takes `node->ty`.

The `typeof` family contributes `OTHER` to `counter`, blocking
combination with primitive specifiers (`int typeof(x)` is an
error).

### B.8 `__builtin_va_list`

Treated as `void *` (i.e., `pointer_to(ty_void)`).  Contributes
`OTHER` to `counter`.

This matches `main`'s ABI choice: ARM64 macOS uses a register-
save-area-pointer ABI for variadics, but at the C type level the
`va_list` is a single pointer, not a struct.  Sources that
include `<stdarg.h>` get the typedef directly; this branch
catches direct `__builtin_va_list` references.

### B.9 Tag specifiers and typedef-name resolution

When `tok` is `struct`, `union`, or `enum`, `declspec` recurses
into `struct_decl` / `union_decl` / `enum_specifier` (§F),
contributes `OTHER` to `counter`, and resumes.

When `tok` is an identifier and `counter == 0` (no primitive
specifier seen yet), `declspec` consults the ordinary namespace
via `find_var(tok)`.  If the result has `type_def != NULL`, the
identifier is a typedef name: `ty` is set to the typedef's
target, `OTHER` is added to `counter`, the token is consumed,
and the loop continues.

The `counter == 0` guard is important: once a primitive specifier
has been seen, identifiers do not start typedef lookups (so
`int x` doesn't try to interpret `x` as a typedef).

### B.10 `__attribute__`

When `tok` is `__attribute__`, `declspec` calls `attribute_list`
(§G).  Before doing so, if the current `ty` is not a tag type
(struct / union / enum), `ty` is `copy_type`'d to avoid mutating
shared primitive type singletons (e.g. `aligned(N)` overwriting
`ty_ulong->align`).

Tag types are not copied because they need to share identity with
the tag table for forward declarations to work.

### B.11 `__extension__`

Silently consumed.  GCC uses this to suppress warnings on
extensions; ncc has no warning pass, so the keyword is a no-op.

### B.12 Post-loop processing

After the loop terminates (when `is_typename(tok)` is false):

1. If `is_atomic || is_const || is_volatile`, copy `ty` and apply
   the flags.
2. If `attr->mode_kind` is set (from `__attribute__((mode(X)))`
   in the attribute list), substitute the type per the mode:
   - `1` (QI / byte): `ty_char` / `ty_uchar`
   - `2` (HI): `ty_short` / `ty_ushort`
   - `4` (SI / word): `ty_int` / `ty_uint`
   - `8` (DI): `ty_long` / `ty_ulong`

   Sign is determined from the **original** type's `is_unsigned`
   flag, OR'd with `(counter & UNSIGNED)`.  The substituted type
   is `copy_type`'d.
3. If `attr->vector_size` is set, convert `ty` to a vector type
   via `vector_of(copy_type(ty), vector_size)`.  Skipped if `ty`
   is already a vector or has `size <= 0`.

The deferred mode and vector_size application is critical: if
applied during attribute parsing, it would corrupt the global
type singletons.  This sequencing is part of the contract.

### B.13 Loop termination

The loop terminates when `is_typename(tok)` returns false.  At
that point, `*rest` is set to `tok` and the constructed `ty` is
returned.

---

## C. Declarator

`declarator(Token **rest, Token *tok, Type *ty)` parses the part
of a declaration after the declspec: pointer stars, optional
nested declarator parens, the identifier, and the type suffix
(array dimensions or function parameters).

### C.1 Pointer parsing (`pointers`)

For each `*` in the input, wrap `ty` in `pointer_to(ty)`.  After
each `*`, accept any sequence of pointer qualifiers:

```
const  volatile  restrict  __restrict  __restrict__  _Atomic
```

Setting `ty->is_const` / `ty->is_volatile` per the matched
qualifier.  `_Atomic` and `restrict`/`__restrict*` are consumed
without setting flags.

After the qualifiers, an optional `__attribute__((...))` is
consumed via `attribute_list` (passing NULL for `attr` — these
attributes attach to the pointer type, not to the declaration's
storage class).

Multiple `*` sequences are processed left-to-right: `int **p`
produces `pointer_to(pointer_to(ty_int))`.

### C.2 Post-pointer attribute consumption

After `pointers`, `declarator` consumes any further
`__attribute__((...))` before the identifier.  Example:

```c
void * __attribute__((noinline)) foo();
```

The attributes attach to the function type via the `ty` in-out
parameter to `attribute_list`.

### C.3 Nested declarator (`( declarator )`)

If the next token is `(` followed by an identifier, `*`, or
another `(`, the parser must distinguish a nested declarator from
a function-parameter list.

The rule (parse.c lines 1157–1198): try parsing the contents of
the parens as a `declarator` against a placeholder type.  If that
parse succeeds and the closing `)` is followed by `[`, `(`, `)`,
`,`, `;`, `=`, or `__attribute__`, it is a nested declarator;
otherwise it is a parameter list.

When it is a nested declarator:
1. Allocate a placeholder `Type` (zero-initialized).
2. Recursively call `declarator` on the inner content with the
   placeholder as the base type.  The recursion produces `inner`.
3. After the closing `)`, parse the trailing `type-suffix` and
   write the result into `*placeholder`.
4. If the recursive call returned a type that was disconnected
   from the placeholder (because `copy_type` ran inside), copy
   the placeholder fields back into `inner` (preserving `inner`'s
   `name` and `name_pos`).
5. Walk the type chain from `inner` down to the placeholder and
   recompute `TY_ARRAY` sizes — array sizes were computed when
   the placeholder was empty (size 0), so they need refresh.

This dance is necessary because the inner declarator's result
type depends on the outer suffix, but the outer suffix is not
known when the inner declarator parses.  The placeholder
indirection threads the dependency.

### C.4 Identifier (terminal)

If the next token is `TK_IDENT`, record it as the declarator's
name.  Otherwise leave `name = NULL` (legitimate for abstract
declarators in casts and `typename_`).

### C.5 Type suffix (`type_suffix`)

Three productions:
- `( ... )` — function parameters via `func_params` (§E).
- `[ ... ]` — array dimensions via `array_dimensions` (§D).
- ε — return current `ty`.

### C.6 Result construction

After the suffix is parsed:
- For `TY_STRUCT`, `TY_UNION`, `TY_ENUM`: set `ty->name` and
  `ty->name_pos` directly on the existing type (no copy).  Tag
  types must share identity with the tag table.
- Otherwise: `copy_type(ty)`, then set `name` and `name_pos` on
  the copy.

### C.7 Abstract declarator

`abstract_declarator` is `declarator` minus the identifier
recognition.  Used by `typename_` (cast targets, `sizeof(T)`,
parameter types).

The nested-paren handling differs slightly: the disambiguation
condition is "next token is not a typename and not `)`."  If
yes, it's a nested abstract declarator; if no, it's a
parameter list.

### C.8 `typename_`

`typename_(Token **rest, Token *tok)` is `declspec` + `abstract_
declarator`.  Used wherever the grammar requires a type-name:
- Cast: `(T)expr`.
- `sizeof(T)`.
- `_Alignof(T)`.
- `_Atomic(T)`.
- Compound literal: `(T){...}`.
- Generic association: `_Generic(x, T: ...)`.
- `__attribute__((mode(T)))` — no, mode takes a keyword, not a
  typename.

`typename_` passes `attr=NULL` to `declspec`, suppressing
storage-class-flag recording (and triggering errors on `_Alignas`
which requires `attr`).

---

## D. Array dimensions and VLA

`array_dimensions(Token **rest, Token *tok, Type *ty)` parses
the contents of `[ ... ]` and returns a derived type.

### D.1 Leading qualifiers and `static`

Per C99, an array-dimension expression in a function parameter
may be preceded by `static`, `const`, `volatile`, `restrict`:

```c
void f(int a[static 10]);
void g(int a[const 10]);
```

These are consumed without semantic effect (ncc does not enforce
the "must be at least N" semantics of `[static N]`).

### D.2 Empty `[]` (incomplete array)

If the next token is `]`, the array is incomplete (`array_len =
-1`).  Recurse into the trailing type-suffix and return
`array_of(rest_ty, -1)`.

### D.3 `[ const-expr ]`

If the dimension expression is a constant per the test in §D.5,
fold via `eval` (the variant of `eval_node` that lives in
parse.c) and return `array_of(rest_ty, val)`.

### D.4 `[ runtime-expr ]` (VLA)

If the dimension expression is not a constant, return
`vla_of(rest_ty, expr_node)`.  Save `dim_start` (the token where
the dimension began) into `vla->vla_dim_tok` for VLA-parameter
side-effect re-parsing in function bodies (used by `04c_stmt.md`
§F.5).

### D.5 Constant-vs-VLA test

The constant test is **structural**, not semantic.  It walks the
expression tree (manual stack, bounded depth 64) looking for
`ND_VAR`, `ND_FUNCALL`, or `ND_STMT_EXPR` nodes.  If any is
found, the expression is treated as non-constant.

Notably, `ND_ADDR`, `ND_MEMBER`, and `ND_DEREF` are **not**
disqualifying — they appear in `offsetof()` patterns like
`&((type*)0)->member`, which are compile-time constants.

This test is more conservative than `eval_node`'s capabilities
(an expression with a `ND_VAR` referencing a constant enum value
would be foldable, but is rejected as non-constant here).  The
conservatism is intentional: `array_dimensions` runs before
`add_type` has fully processed the expression's enum-constant
substitutions, and a false negative just produces a VLA where a
fixed array would have been valid — codegen handles both.

### D.6 VLA hidden-local emission

The hidden-local size variable is **not** emitted by
`array_dimensions`.  It is emitted later by `compute_vla_size`
(§J.4) when the declaration is processed by `declaration` (§H)
or as part of K&R parameter type adjustment.

This separation of concerns matters: `array_dimensions` is
called from many contexts (function parameter lists, cast
targets via `typename_`, struct member declarations), and only
some of those want runtime size emission.

### D.7 Multi-dimensional arrays

`int a[N][M]` parses as `array_of(array_of(ty_int, M), N)`.  The
recursion is through `type_suffix`: outer `[N]` calls
`array_dimensions`, which recurses into `type_suffix` for the
trailing `[M]`, which calls `array_dimensions` again.

For multi-dimensional VLAs, `compute_vla_size` recurses into
`ty->base` (§J.4) so each dimension's runtime size is computed
from inside out.

---

## E. Function prototypes

`func_params(Token **rest, Token *tok, Type *ty)` parses the
contents of `( ... )` in a function declarator and returns a
`TY_FUNC` type.

### E.1 Empty parameter list `()`

Treated as a K&R-compatible declaration: returns `func_type(ty)`
with `params = NULL` and `is_variadic = true` (per the loop's
final adjustment when `cur == &head`).

This matches GCC's permissive treatment.  C11 strictly says
`()` declares a function taking unspecified parameters; ncc's
choice here is to mark variadic so `va_start` works correctly
in K&R-style bodies.

### E.2 `(void)`

Special-case: explicitly empty parameter list.  Returns
`func_type(ty)` with `params = NULL` and `is_variadic = false`.

The detection is `equal(tok, "void") && equal(tok->next, ")")`.
Note this is a two-token lookahead — but only after the parser
has decided that the `(` opens a parameter list.

### E.3 Normal parameter list

Push a scope (so that VLA dimensions in later parameters can
reference earlier parameter names), then loop:

1. If first iteration, no leading comma; otherwise consume `,`.
2. If next token is `...`, set `is_variadic = true` and break.
3. Parse declspec (with NULL `attr`).
4. Parse declarator.
5. Consume any trailing `__attribute__((...))` on the parameter.
6. If the parameter has a name, push it into the scope so later
   parameters can reference it (e.g., `int f(int n, int a[n])`).
7. Apply parameter type adjustments (§E.4).
8. `copy_type` the parameter, append to the parameter list.

After the loop, leave_scope.

If the loop saw zero parameters (only `...` or empty), set
`is_variadic = true`.  Otherwise `is_variadic` reflects whether
`...` appeared.

### E.4 Parameter type adjustments

C11 §6.7.6.3:
- An array parameter (`TY_ARRAY` or `TY_VLA`) decays to a pointer
  to its element type.
- A function parameter (`TY_FUNC`) decays to a pointer to that
  function type.

Additional ncc-specific behavior: when a `TY_VLA` parameter
decays to a pointer, the original `vla_dim_tok` is preserved on
the resulting pointer type.  This lets the function body re-parse
the dimension expression to evaluate its side effects (per
`04c_stmt.md` §F.5).

### E.5 Parameter scope leakage

The parameter scope pushed at the top of `func_params` is popped
before returning, **before** the function body's scope is
pushed.  Parameter names re-enter the function-body scope when
`function` (§I) iterates over `fn->params` and calls `new_lvar`.

This means a parameter name is visible inside the parameter list
(for VLA dimensions) and inside the function body, but not in
default-argument-position-like contexts (C has none).

### E.6 K&R-style function definitions (§E.2 of skeleton, expanded)

ncc accepts K&R-style function definitions:

```c
int foo(a, b)
  int a;
  char *b;
{
  ...
}
```

Detection happens at the top-level entry (`parse`, §I), not in
`func_params`.  When `parse` sees a function declarator followed
by a type keyword (and not `;`, `__attribute__`, or `__asm__`),
it dispatches to `function` for a K&R-style body.

Inside `function`, the K&R parameter type declarations are parsed
and matched to the previously-recorded parameter slots: each
declarator's name is looked up in `fn->params`, and the matching
`Obj`'s type is updated.

This is the only K&R surface ncc supports.  K&R prototype-less
function declarations (`int foo()` meaning "any args") are
covered in §E.1.

---

## F. Struct, union, and enum

### F.1 Tag namespace

Struct, union, and enum tags share a tag namespace per scope.
They are kept separate from the ordinary namespace per C11
§6.2.3.  The same identifier can name a tag and an ordinary
object simultaneously:

```c
struct foo { int x; };
int foo;          // OK — different namespaces
sizeof(struct foo);   // 4
sizeof(foo);          // 4 (the int variable)
```

`find_tag(tok)` walks the scope chain inward to outward; the
`TagScope` record stores `name` (interned by string) and the
associated `Type *`.

### F.2 `struct_decl` / `union_decl` flow

Both follow the same shape (parse.c lines 1584–1690):

1. Parse leading `__attribute__((...))` into a temporary `Type
   early_attr` slot — captures `packed`/`aligned` placed before
   the tag name.
2. If next token is `TK_IDENT`, record as `tag` and consume.
3. Parse trailing `__attribute__((...))` again into `early_attr`
   — captures attributes between tag name and body.
4. **Forward declaration case:** if `tag` is set and next token
   is not `{`, look up `tag` in tag namespace.  If found, return
   it; otherwise create a fresh `struct_type()` with `size = -1`
   (incomplete), push to tag scope, return.
5. **Definition case:** consume `{`, then:
   - If `tag` exists and the existing type is incomplete (or no
     existing type), reuse / create.  If `tag` exists and the
     existing type is complete, create a new type (struct
     re-definition is permitted for nested-scope shadowing).
   - Apply `early_attr` (packed, aligned).
   - Parse `struct_members` (§F.3).
   - Run `struct_layout` or `union_layout` (§F.4–F.5).
   - Parse trailing `__attribute__((...))`.
   - If trailing attributes set `is_packed` and the prior layout
     was non-packed, re-run `struct_layout` — the first pass
     inserted padding that packed layout shouldn't have.

### F.3 `struct_members`

Loops until `}`, parsing one member-declaration per iteration.
Each member-declaration is a `declspec` + comma-separated
init-declarators, followed by `;`.

Special cases:
- `_Static_assert(...)` and `static_assert(...)` inside a struct
  body: parsed, evaluated, discarded.  No member emitted.
- Anonymous member: a member-declaration whose declspec is a
  struct or union type and whose declarator list is empty (just
  `;` after declspec).  Records a `Member` with `name = NULL`,
  type `basety`.  Anonymous members participate in member lookup
  via `find_member`'s embedded-struct walk (`04b_expr.md` §H.2).

For each member declarator:
- Parse declarator against `basety`.
- If next token is `:`, parse a bit-field width via
  `const_expr_val`; set `is_bitfield = true`.
- Consume optional trailing `__attribute__((...))` into a
  per-member `VarAttr` (so `aligned(N)` updates the member's
  `align`, not the shared type's).

### F.4 Flexible array member

If the last member in a struct is `TY_ARRAY` with `array_len <
0` (incomplete), it is rewritten to `array_of(base, 0)` and
`ty->is_flexible = true`.  This is the C99 flexible array
member: `struct S { int n; char data[]; }`.

`is_flexible` enables size-from-init computation when the struct
is initialized (`04d_init.md` §A.3).

### F.5 Struct layout (`struct_layout`)

Computes member offsets, total size, and alignment.

For non-bitfield members, when the struct is not packed:
- Align `offset` up to `mem->align`.
- Set `mem->offset = offset`.
- Increment `offset` by `mem->ty->size`.
- Track `max_align`.

When the struct is packed (`is_packed`):
- Skip the per-member alignment.  Bit-counter `bits` is aligned
  to byte boundary (`bits = align_to(bits, 8)`) for non-bitfield
  members.

For bitfield members:
- Use a running `bits` counter (in bits, not bytes) within a
  storage unit of size `mem->ty->size`.
- If transitioning from non-bitfield to bitfield, sync `bits`
  with `offset * 8`.
- A zero-width bitfield (`bit_width == 0`) aligns `bits` to the
  next storage-unit boundary; no slot is allocated.
- Otherwise, if the bitfield does not fit in the current storage
  unit, advance `bits` to the next unit.
- `mem->offset` is the byte offset of the storage unit;
  `mem->bit_offset` is the within-unit bit offset.
- Advance `bits` by `mem->bit_width`.
- Update `offset` to `(bits + 7) / 8`.

After all members, set `ty->align = max_align` (or 1 if packed)
and `ty->size = align_to(offset, max_align)` (or `offset` if
packed).

### F.6 Union layout (`union_layout`)

All members at offset 0.  Union size is the maximum member size,
aligned up to the maximum member alignment.  No bitfield-specific
handling — bitfields in unions occupy their full storage unit.

### F.7 `enum_specifier`

Returns a `TY_ENUM` type.  ncc treats enum constants as `int`
unconditionally — no underlying-type inference based on values
(this is a known simplification vs C23).

Forms:
- `enum Tag { ... }` — full definition.
- `enum Tag` — forward reference; permitted (creates an
  incomplete enum type if not yet defined).
- `enum Tag : underlying-type { ... }` — C23 / Objective-C fixed
  underlying type; ncc parses and discards the underlying type.

The body is `{ enumerator [ , enumerator ]* [ , ] }` where each
enumerator is `name [ = const-expr ]` followed by an optional
trailing `__attribute__((...))` (for clang's `CURL_DEPRECATED`
etc., consumed and ignored).

Each enumerator pushes a `VarScope` with `enum_ty` set to `ty`
and `enum_val` set to the running value.  Subsequent identifier
lookups find the enumerator in the ordinary namespace.

The running value starts at 0; `= const-expr` overrides it; each
enumerator post-increments after its value is recorded.

### F.8 Anonymous struct/union members (GCC extension)

```c
struct Outer {
  int a;
  struct {
    int b;
    int c;
  };
  int d;
};
struct Outer x;
x.b = 1;   // direct access to nested member
```

The anonymous struct contributes its members to the outer
struct's lookup namespace.  Implementation: the anonymous member
has `name = NULL`; `find_member` (`04b_expr.md` §N) recurses
into anonymous members when searching by name, returning a
chain of offsets so codegen can reach the deeply-nested member.

---

## G. `__attribute__` parsing (`attribute_list`)

`attribute_list(Token *tok, Type *ty, VarAttr *attr)` consumes
zero or more `__attribute__((...))` clauses, optionally followed
by an `__asm__("...")` label, and returns the new `tok`.

### G.1 Skip `__asm__` label

`skip_asm_label(tok)` is called first.  If `tok` is `asm`,
`__asm__`, or `__asm`, consume it and the parenthesized body
(brace-balanced consumption).  The label string itself is
currently discarded — `main` does not yet honor explicit asm
labels in symbol emission.

(The honoring path is a known divergence-log item; not blocking
Phase 4 closure.)

### G.2 Attribute clause structure

```
__attribute__ ( ( attribute [ , attribute ]* ) )
```

The double parens are mandatory.  Inside, attributes are
comma-separated; trailing comma not permitted.

Each attribute is a name (possibly followed by `(` arglist `)`).
The name is matched against the disposition table in
`04_parse.md` §11.

### G.3 Honored attributes

| Name | `ty` mutation | `attr` mutation |
|---|---|---|
| `packed`, `__packed__` | `ty->is_packed = true` | — |
| `aligned(N)`, `__aligned__(N)` | `ty->align = N`; `ty->size` rounded up | `attr->align = N` |
| `aligned`, `__aligned__` (no args) | `ty->align = 16` | `attr->align = 16` |
| `vector_size(N)`, `__vector_size__(N)` | (deferred) | `attr->vector_size = N` |
| `mode(M)`, `__mode__(M)` | (deferred) | `attr->mode_kind = K` |
| `alias("name")`, `__alias__("name")` | — | `attr->alias_target = "name"` |

The `vector_size` and `mode` deferral is noted in §B.12: applied
post-loop in `declspec` to avoid corrupting global type
singletons.

A subtle case: when `attribute_list` is called directly with
`ty` set and `attr == NULL` (e.g., on a pointer suffix from
`pointers()`), `vector_size` is applied immediately to the
`ty` (with `*ty = *vec` after backing up `name`/`name_pos`).
This in-place mutation matters for sites where the caller relies
on `ty` to be a non-singleton.

### G.4 Section, visibility, alias

- `section("name")`, `__section__("name")` — currently parsed
  and the contents skipped (no `attr` field is set).  This is a
  known incompleteness vs `main`'s codegen-honored `section`
  attribute; the `Obj.section` field exists, but the wiring from
  attribute_list to that field is via the per-attribute
  `attr->section` slot which is not currently populated here.
- `visibility("name")` — parsed and discarded.
- `alias("target")`, `__alias__("target")` — sets
  `attr->alias_target`.

### G.5 Parsed-and-ignored attributes

The following are recognized by name and consumed (with optional
`( arglist )` skipped via brace-balanced loop):

```
unused, __unused__, weak, may_alias, noinline, noclone, noreturn,
nothrow, pure, const, deprecated, used, warn_unused_result,
always_inline, cold, hot, malloc, flatten, constructor, destructor,
transparent_union, returns_nonnull
```

And:

```
format, __format__, sentinel, alloc_size, cleanup, nonnull
```

(These take optional arguments; the args are skipped.)

Note: `weak`, `used`, `noreturn`, and `transparent_union` appear
on this list as parsed-and-ignored.  Per the disposition table
in `04_parse.md` §11, `weak`, `used`, and `noreturn` are listed
as HONORED.  This is a discrepancy worth tracking — the table in
§11 promises codegen wiring that the current `attribute_list`
does not deliver.  See §N below.

### G.6 Unknown attributes

Any attribute name not matched above is consumed (with optional
arg list skipped) without error.  This is GCC's convention for
forward compatibility with newer attributes.

### G.7 Comma handling

After each attribute, if the next token is not `)`, expect `,`.
Unmatched comma or missing comma between attributes is an
error.

---

## H. Local declarations (`declaration`)

`declaration(Token **rest, Token *tok, Type *basety, VarAttr *
attr)` parses a single statement-context declaration: one or
more init-declarators after a previously-parsed `basety` /
`attr`.  Returns an `ND_BLOCK` node containing the initialization
statements (and any VLA size-computation statements).

### H.1 Type-level VLA precomputation

Before the per-declarator loop, if `basety` itself contains a
VLA member (e.g., a struct type whose definition includes a VLA
field) and `basety->vla_size` has not yet been computed, call
`compute_vla_size(basety, tok)` (§J.4) to emit the size-
computation statement and record the size variable.

This handles the case where the `basetype` already describes a
struct-with-VLA, and the size needs to be available at this
declaration point.

### H.2 Per-declarator loop

Iterates while not at `;`:
- After the first iteration, expect `,`.
- Parse declarator against `basety`.
- If type is `TY_VOID`:
  - With `attr->is_extern`: rewrite to `ty_char` (extern void as
    a linker symbol placeholder).
  - Otherwise: error "variable declared void".
- Require declarator to have a name; error otherwise.
- Consume trailing `__attribute__((...))` into the same `attr`.
- Branch on storage class:

### H.3 `static` local

Lift to anonymous file-scope global via `new_anon_gvar`.
`push_scope` records a binding in the local scope so the
identifier resolves to the global.  If `=` follows, parse
`gvar_initializer` (`04d_init.md` §G).

### H.4 `extern` local

Create a `new_gvar(name, ty)` with `is_definition = false`.  The
linker resolves at link time.  No local storage allocated.

### H.5 Normal local

Create `new_lvar(name, ty)`.  Apply `attr->align` if non-zero.
If `=` follows, parse `lvar_initializer` (`04d_init.md` §A) and
append the resulting node to the block's body.

If the local has VLA storage (kind `TY_VLA`, or struct/union
containing VLAs), emit the VLA setup sequence:
1. If `ty->vla_size` is not yet computed, call `compute_vla_size`
   and append the resulting statement.
2. Allocate a hidden `saved_sp` local of `pointer_to(ty_char)`
   to hold the pre-allocation stack pointer.
3. Emit an `ND_VLA_PTR` node referencing both `var` and
   `saved_sp` (via `lhs`).  Codegen lowers this to:
   - Restore SP from `saved_sp` (if non-zero) — for goto-loop
     re-entry.
   - Save current SP into `saved_sp`.
   - Allocate the VLA byte size.
   - Store the new pointer into `var`.

If the local has incomplete type (`size < 0`) and is not
otherwise handled, error "variable has incomplete type".

### H.6 Output

After the loop, consume `;` (which is the terminator that
exited the loop).  Build an `ND_BLOCK` node with `body` set to
the head of the accumulated statement chain.  Return.

---

## I. Top-level entry (`parse`)

`Obj *parse(Token *tok)` is the only externally-visible parser
entry point per `04_parse.md` §2.

### I.1 Setup

- Initialize `globals = NULL`.
- Push a fresh top-level scope via `enter_scope`.

### I.2 Top-level loop

Loop until `tok->kind == TK_EOF`.  Each iteration handles one
top-level construct.

**Stray attributes:** if `tok` is `__attribute__`, consume the
attribute list and any trailing `;`.  Used for file-scope
GCC pragmas-as-attributes that don't bind to a declaration.

**Stray semicolons:** if `tok` is `;`, consume.

**`_Static_assert` / `static_assert` at file scope:** parse
expression via `const_expr_val`, optional message string, `)`,
`;`.  If value is zero, print message (if any) and error.
Otherwise no AST output.

**Otherwise: a declaration.**
1. Parse `declspec` with `attr`.
2. If `attr.is_typedef`, dispatch to `parse_typedef`.
3. Otherwise, parse `declarator` against `basety`.
4. If the resulting type is `TY_FUNC`, dispatch on the next
   token:
   - `{` — function definition; call `function`.
   - Type-keyword (and not `;`, `__attribute__`, `__asm__`/
     `__asm`/`asm`) — K&R-style function definition; call
     `function` (which handles the K&R declarators).
   - Otherwise — function declaration.  Loop over comma-separated
     declarators; for each, create a non-defining `Obj`, consume
     optional `__asm__` label and `__attribute__`.  Skip `;`.
5. If the type has no name (bare struct/union/enum definition),
   skip `;` and continue.
6. Otherwise, dispatch to `global_variable` for global variable
   processing.

### I.3 Cleanup

After the loop, `leave_scope` and return `globals`.

The `Obj` chain returned from `parse` is the head of the global
list, in reverse declaration order (each `new_gvar` prepends).
Codegen iterates in this order; ABI conventions for emission
order are codegen's concern.

### I.4 `parse_typedef`

(Defined elsewhere in `parse.c`; not the focus of this section.)
Parses comma-separated declarators after a `typedef` declspec
and registers each as a typedef in the ordinary namespace via
`push_scope` with the declarator's resulting type stored in
`type_def`.

### I.5 `function`

Receives the function declarator's type and `VarAttr`.
Responsibilities:
1. Look up prior declaration (if any) to inherit alignment.
2. Create the defining `Obj` via `new_gvar`; set
   `is_function`, `is_definition`, `is_static`, `is_extern`,
   `is_inline`, `is_variadic`.
3. Reset per-function state: `current_fn`, `locals`, `gotos`,
   `labels`, `brk_label`, `cont_label`, `current_switch`.
4. `enter_scope`.
5. Re-create parameter `Obj`s in the function's scope.  Process
   in reverse so `new_lvar` (which prepends) yields the right
   order.  Unnamed parameters get a generated unique name.
6. Set `fn->params = locals`.
7. Handle K&R-style parameter type declarations between `)` and
   `{` (§E.6).
8. Parse the function body via `compound_stmt`.
9. Run `add_type` on the body.
10. Resolve forward `goto` targets against the labels list.
11. `leave_scope`.

The detailed body-parsing path is in `04c_stmt.md` §F.

### I.6 `global_variable`

Receives the parsed type and `VarAttr`.  Responsibilities:
1. Loop over comma-separated declarators (continuing from the
   first one parsed in `parse`).
2. For each, consume `__asm__` label and `__attribute__` after
   the declarator.
3. Create `Obj` via `new_gvar`; set `is_static`, `is_tls`,
   `is_definition` (false if `extern`).
4. If `=` follows, parse `gvar_initializer` (`04d_init.md` §G).
5. After the last declarator, skip `;`.

---

## J. Variable creation helpers

### J.1 `new_var(name, ty)`

Allocates a zero-initialized `Obj`, sets `name`, `ty`, `align =
ty->align`, pushes into the current scope as an ordinary-
namespace binding (`push_scope(name)->var = var`), returns the
`Obj`.

Used as the building block for `new_lvar` and `new_gvar`.

### J.2 `new_lvar(name, ty)`

Calls `new_var`, sets `is_local = true`, prepends to the file-
scope `locals` chain, returns the `Obj`.

The `locals` chain is reset at function entry by `function`
(§I.5) and finalized as `fn->locals` at function exit (computed
implicitly: every new_lvar during body parsing has accumulated
the chain).

### J.3 `new_gvar(name, ty)` and `new_anon_gvar(ty)`

`new_gvar`: Calls `new_var`, prepends to `globals`, sets
`is_static = false`, `is_definition = true`.

`new_anon_gvar`: Calls `new_gvar` with a generated `.L.data.N`
name, then sets `is_static = true`.  Used for string literals,
`static`-promoted locals, anonymous globals.

The `gvar_cnt` counter is process-wide; per `04_parse.md` §3,
this is the only Node-shape difference permitted between v1 and
v2 parsers.  Self-host fixed-point survives because each
compilation pass starts from `gvar_cnt = 0`.

### J.4 `compute_vla_size(ty, tok)`

Emits the runtime size-computation statement for a VLA type.
Called from `declaration` (§H) and from K&R parameter
adjustments.

For `TY_VLA`:
1. Recursively compute base VLA size first (multi-dimensional
   case).
2. Allocate hidden local `size_var` of type `ty_ulong`.
3. Compute `vla_len * base_size`:
   - If base is a VLA: `base_size = new_var_node(ty->base->vla_
     size, tok)`.
   - If base has a `vla_size` (struct-with-VLA): same.
   - Otherwise: `base_size = new_ulong(ty->base->size, tok)`.
4. Cast `vla_len` to `ty_ulong`.
5. Multiply.
6. Assign to `size_var`.
7. Wrap in `ND_EXPR_STMT`.
8. If the recursive base call produced a statement, chain: outer
   block contains both base-stmt and current-stmt.

For struct/union containing VLA:
1. Recurse into each member to emit its size statement.
2. Allocate hidden `size_var` of `ty_ulong`.
3. Compute total size:
   - Struct: last-member-offset + last-member-vla-size (if last
     member is VLA), or `ty->size` if no VLA member is last.
   - Union: max of member sizes (with the first VLA member's
     runtime size taking precedence).
4. Assign to `size_var`.
5. Set `ty->vla_size = size_var`.
6. Wrap chain in an `ND_BLOCK` if more than one statement.

The insertion point of the returned statement is the caller's
responsibility — `declaration` (§H) appends it to the block
body before the variable's own initialization, per `04_parse.md`
§7.3.

---

## K. `Obj` data shape

The fields of `Obj` populated by this sub-chunk's parsing paths:

| Field | Set by | Meaning |
|---|---|---|
| `next` | `new_lvar`, `new_gvar` | Chain pointer (locals or globals). |
| `name` | `new_var` | Symbol name. |
| `ty` | `new_var` | Type. |
| `tok` | (set by some Node creators; `Obj` itself does not always have it) | Representative token for diagnostics. |
| `is_local` | `new_lvar` | true for locals. |
| `align` | `new_var` (initial), `declaration` / `__attribute__((aligned(N)))` (override) | Alignment. |
| `offset` | codegen (Phase 5) | Frame-pointer offset; not parser's concern. |
| `is_function` | `parse` / `function` | True for function `Obj`s. |
| `is_definition` | `function` (true), forward-decl path (false), `extern`-local (false) | Whether body is present. |
| `is_static` | `function`, `global_variable` from `attr->is_static`; `new_anon_gvar` sets true | C `static` storage class. |
| `is_extern` | `function`, `global_variable` from `attr->is_extern` | C `extern` storage class. |
| `is_tentative` | (rare; tentative-definition handling) | C tentative-definition flag. |
| `is_tls` | `global_variable` from `attr->is_tls` | Thread-local. |
| `is_inline` | `function` from `attr->is_inline` | Inline function. |
| `init_data`, `init_data_size`, `rel` | `gvar_initializer` (`04d_init.md` §G) | Static initializer bytes + relocations. |
| `section` | (currently unused by parser; see §G.4) | Section override. |
| `visibility` | (currently unused by parser) | Visibility. |
| `alias_target` | populated from `attr->alias_target` by `function` / `global_variable` | Alias attribute target. |
| `params` | `function` (set to `locals` after parameter creation) | Parameter list. |
| `body` | `function` from `compound_stmt` | Function body AST. |
| `locals` | `function` (set to `locals` after body parse) | All locals in the function. |
| `va_area` | codegen / variadic setup | `__va_area__` for variadics. |
| `alloca_bottom` | codegen | `__alloca_bottom__`. |
| `stack_size` | codegen | Set after stack layout. |
| `is_variadic` | `function` from `fn_ty->is_variadic` | Variadic flag. |
| `is_nested`, `chain_param`, `enclosing_fn`, `nlgoto_targets` | nested-function path (`04c_stmt.md` §F.4) | Nested function support. |
| `is_live`, `is_root`, `refs` | `function` / dead-code analysis | Static-inline-function liveness. |

The reverse direction — what codegen reads — is documented in
`04_parse.md` §15.

---

## L. Worked examples

### L.1 `static const int x = 42;` (file scope)

1. `parse` reads `static const int`.  `declspec` with `attr`:
   - `static` → `attr->is_static = true`.
   - `const` → `is_const = true` accumulator.
   - `int` → `counter = INT`; switch arm sets `ty = ty_int`.
   - Loop terminates on `x` (not in `is_typename`).
   - Post-loop: copy `ty_int`, set `is_const`.
2. `declarator(rest, "x", ty)`:
   - No `*`, no nested paren.
   - Identifier `x` recorded.
   - No type suffix.
   - `copy_type` (already a copy from declspec, so a second copy
     is harmless), set `name = x`.
3. `parse` sees `=`, dispatches to `global_variable`, which calls
   `gvar_initializer`.  Result: `Obj` with `init_data` containing
   `[0x2a, 0, 0, 0]` (little-endian), no relocations.
4. `Obj` flags: `is_static = true`, `is_definition = true`,
   `name = "x"`, `ty->kind = TY_INT`, `ty->is_const = true`.

### L.2 `int (*f)(int, int);`

1. `declspec` produces `ty_int`.
2. `declarator(rest, "(*f)(int, int)", ty_int)`:
   - `pointers` sees no leading `*`.
   - Next token is `(` followed by `*`, which is a candidate for
     nested declarator.  Try-parse: `*f` parses as a declarator
     (returns pointer-to-placeholder).  After the closing `)`,
     next token is `(` — which is in the OK-set.  Confirmed
     nested.
   - Allocate placeholder.  Recursively call
     `declarator("*f", placeholder)`:
     - `pointers` sees `*`, wraps placeholder in pointer.
     - Identifier `f`.
     - No suffix.
     - Returns pointer-to-placeholder with `name = f`.
   - Skip `)`.
   - Parse outer suffix: `( int, int )` → `func_params` returns
     `func_type(ty_int)` with two int params.
   - Write that into placeholder.
   - Inner type now references placeholder, which is the function
     type.  Final type: pointer-to-(function returning int taking
     two ints).
3. `Obj` `f`: type = function-pointer, no body.

### L.3 `struct S { int a; int b : 5; int c : 3; } s;`

1. `declspec` sees `struct`:
   - `struct_decl` with no leading attributes.
   - Tag `S` consumed.
   - `{` consumed.
   - `struct_members`:
     - Member 1: declspec=int, declarator="a", no bitfield.
       offset (after layout) = 0.
     - Member 2: declspec=int, declarator="b", `:5`.
       `is_bitfield = true`, `bit_width = 5`.
     - Member 3: declspec=int, declarator="c", `:3`.
       `is_bitfield = true`, `bit_width = 3`.
   - `struct_layout`:
     - `a`: non-bitfield, `align=4`, offset 0, size 4.  `offset
       = 4`, `bits = 0`, `max_align = 4`.
     - `b`: bitfield in 4-byte unit.  `bits` synced to `4*8 =
       32`.  Cur unit ends at 64.  Fits; `b->offset = 4`,
       `b->bit_offset = 0`.  `bits = 37`.
     - `c`: bitfield in 4-byte unit.  Cur unit ends at 64.  37 +
       3 = 40 ≤ 64.  Fits; `c->offset = 4`, `c->bit_offset = 5`.
       `bits = 40`.
     - End: `offset = 5`, `align_to(5, 4) = 8`.  `ty->size = 8`,
       `ty->align = 4`.
2. `declarator(rest, "s", struct_S)`: sets `name=s` on
   `struct_S`'s type (no `copy_type` for tag types).
3. `Obj` `s`: type = struct S (size 8, align 4), is_local
   depending on context.

### L.4 `int a[n][m];` where `n`, `m` are runtime variables

1. `declspec` produces `ty_int`.
2. `declarator(rest, "a[n][m]", ty_int)`:
   - No `*`, no nested paren.
   - Identifier `a`.
   - `type_suffix` sees `[`:
     - `array_dimensions(rest, "n][m]", ty_int)`:
       - No leading `static`/qualifier.
       - Not `]` (so not incomplete).
       - Parse `n` via `cond_expr` → `ND_VAR` referencing `n`.
       - Constant test: walks tree, sees `ND_VAR`, returns false.
       - VLA: `vla = vla_of(rest_ty, expr_node)`.
       - Recurse for `[m]`: same flow, gives `vla_of(ty_int,
         ND_VAR(m))`.  That's `rest_ty`.
       - Outer: `vla_of(rest_ty, ND_VAR(n))`.
   - `copy_type` not applied for ground types... actually for
     non-tag types it is.  Set name = a.
3. `declaration`:
   - Type is `TY_VLA`.
   - `compute_vla_size(ty, tok)`:
     - Recurse into `ty->base` (the inner VLA).  Returns its
       size statement: `__vla_size_1 = (ulong)m * 4`.
     - Outer: allocate `__vla_size_2`.  base_size =
       `new_var_node(__vla_size_1)`.  Compute `__vla_size_2 =
       (ulong)n * __vla_size_1`.
     - Chain both statements in an `ND_BLOCK`.
   - Allocate `saved_sp` local.
   - Emit `ND_VLA_PTR` with `var = a`, `lhs = saved_sp`.
4. Block body: [size-block, vla-ptr-stmt, ...].

### L.5 `typedef int (*signal_handler_t)(int);`

1. `declspec` with `attr`: `typedef` → `attr->is_typedef = true`.
   `int` → counter=INT, ty = ty_int.
2. `parse` sees `attr.is_typedef`, dispatches `parse_typedef`.
3. `parse_typedef` parses `(*signal_handler_t)(int)` as a
   declarator (per §C.3 nested-paren handling).  Result type:
   pointer-to-(function returning int taking int), with
   `name = signal_handler_t`.
4. `parse_typedef` calls `push_scope(name)` and stores the type
   in the resulting `VarScope.type_def`.  No `Obj` emitted.
5. Subsequent `is_typename` calls on tokens spelled
   `signal_handler_t` find this binding and return true.

---

## M. Phase 5 prerequisites added by 04a

Append to `04_parse.md` §15 master list during impl review:

1. **VLA `Obj`s appear as ordinary locals.**  `__vla_size_N`
   variables (the hidden size locals created by
   `compute_vla_size`) and `saved_sp` variables (created by
   `declaration` for `TY_VLA` locals) are normal `is_local =
   true` `Obj`s with type `ty_ulong` / `pointer_to(ty_char)`.
   Codegen sees them like any other local.

2. **`ND_VLA_PTR` Node kind.**  Codegen receives this node with
   `var` set to the VLA's `Obj` and `lhs` an `ND_VAR` referencing
   the saved-sp local.  Lowering: restore SP if non-zero, save
   current SP, allocate, store pointer.

3. **`is_flexible` flag on struct types.**  Codegen size
   computations for objects with flexible array members rely on
   `init_data_size`, not `ty->size`, when `init_data_size >
   ty->size`.

4. **Bit-field offset/bit_offset/bit_width/ty->size.**  Codegen
   bit-field load and store paths use these four values.  The
   parser's job (per §F.5) is to populate them per the layout
   rules; codegen reads them as-is.

5. **`Obj.params` is the linked list in declaration order.**  The
   reverse-then-prepend trick in `function` (§I.5) ensures this.
   ABI lowering iterates `params->next->next...` and assigns
   register/stack slots in that order.

6. **`is_variadic` is set on the function `Obj`, not just the
   `Type`.**  Codegen checks both; the parser sets both.

---

## N. Open questions

These are sub-chunk-specific items raised during drafting.
Resolutions are tracked in this section; if any escalate to a
top-level Phase 4 design Q, they migrate to
`04_parse_questions.md`.

### N.1 Attribute disposition table discrepancy

The disposition table in `04_parse.md` §11 lists `weak`, `used`,
and `noreturn` as HONORED.  The current `attribute_list`
implementation (§G.5) routes them through the parsed-and-ignored
arm — the codegen wiring promised by the table is not present.

Two consistent resolutions:
- **(a)** Implement the wiring during Phase 4: add
  `attr->is_weak`, `attr->is_used`, `attr->is_noreturn` fields,
  populate from these attributes, and have codegen honor.
- **(b)** Move these attributes to PARSED-AND-IGNORED in §11 of
  `04_parse.md`, matching current behavior.

Recommended: **(a)**, since real source code (e.g., libc
headers, kernel-style code) relies on `weak` and `noreturn`
being HONORED.  `noreturn` already has a separate path via
`_Noreturn` keyword; the `__attribute__((noreturn))` form
should match.

This is a Phase 4 implementation task, not a spec ambiguity.
The spec (§11 of `04_parse.md`) is correct as written;
implementation must catch up.

### N.2 `__asm__` label honoring

`skip_asm_label` (§G.1) currently discards the asm label string.
The `Obj.alias_target` field is for the `__attribute__((alias))`
form, not for `__asm__("...")` labels.

Phase 4 should either:
- Wire `__asm__("name")` labels to override the symbol name in
  `Obj.name`.
- Document the omission in §13 of `04_parse.md` as an intentional
  divergence.

Recommended: wire it.  Real source code (musl, glibc-headers-via-
posix) relies on `__asm__` labels.

### N.3 K&R parameter-promotion semantics

`function` (§I.5) updates parameter types from K&R declarations.
A subtle case: a K&R parameter declared as `char x;` should be
promoted to `int` in the function's type signature (per C11
§6.7.6.3/8 — old-style function declarators promote function
arguments).

Current behavior (verify during impl): `main` does **not**
promote — the parameter type stays `char`.  This matches GCC's
default behavior when the prototype is not in scope.

Phase 4 disposition: preserve current behavior.  If a regression
test surfaces, document in §13.
