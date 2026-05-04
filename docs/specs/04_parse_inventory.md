# ncc Parser — Phase 4 Inventory

This is a structural audit of `src/parse.c` (6136 lines) on the
`swap-out` branch (tip `d582d38`, post `phase-3-closed`).
Inventory, not spec.  The spec (`04_parse.md`) is the user-
collaborative next step that comes after this document.

Phase 4 is materially different from Phases 1–3 in scale: parse.c
is the largest single file in the compiler (~3.4× preprocess.c,
~14× type.c).  Per project memory `project_ncc_swapout.md`,
"Parser (Phase 4) is the big one (~6k lines) and gets broken
into subsections."  Accordingly, this inventory is a **structural
roadmap** keyed off the file's section-comment markers, not a
function-by-function listing (parse.c has ~159 static functions
plus the public surface; per-function detail would obscure rather
than clarify).

The intent is to identify (a) the spec's top-level subdivisions,
(b) cross-cutting concerns that span subsections, (c) data shapes
the parser produces, and (d) gotchas worth flagging before spec
authoring begins.

---

## 1. Public surface

Just two exported symbols (declared in `cc.h`):

| Function | Line | Purpose |
|---|---|---|
| `parse(Token *tok)` | 5552 | Top-level entry. Builds an AST from a preprocessed token stream; returns a linked list of `Obj` (functions and globals). |
| `new_cast(Node *expr, Type *ty)` | (within parse.c) | Wrap an expression in a typed cast. Used by `type.c`'s `usual_arith_conv` and by parse.c's own substitution paths. |

Plus two evaluator helpers also declared in `cc.h`:
- `eval_node(Node *node)` — fold a constant expression to `int64_t`.
- `try_eval_node(Node *node, int64_t *out)` — non-fatal version.

Everything else is `static`.

---

## 2. Structural map (by file section)

The section-comment markers in `src/parse.c` partition the file
into ~24 zones.  Each zone is a candidate spec subsection.

| Lines | Section | Purpose |
|---|---|---|
| 6–113 | Scope management | `Scope` records (typedef + tag + ordinary scopes), `enter_scope`/`leave_scope`, `find_var`/`find_tag`. |
| 32–50 | Scoped object lists | break/continue target tracking. |
| 51–72 | Nested function support | captured outer variables for nested functions (GCC extension). |
| 109–113 | Label name minting | unique label name generation. |
| 115–163 | Node construction | `new_node`, `new_unary`, `new_binary`, `new_num`, `new_var_node` etc. |
| 164–399 | Complex / vector helpers | `__real__` / `__imag__` constructors, vector element extract/set, vector binary/unary decomposition. |
| 400–462 | Variable creation + complex value construction | `new_lvar`, `new_gvar`, `new_anon_gvar`, complex tmp-var pattern. |
| 463–614 | Variable attributes + VLA helpers | `apply_attr`, VLA size-computation generation. |
| 616–676 | Type handling utilities | typedef resolution, type-name detection. |
| 678–700 | Type-name detection | the `is_typename` check used everywhere parsing branches on "is this a type or an expression?" |
| 702–959 | **Declaration specifiers** | the heart of declaration parsing.  Combines storage class, type specifiers, qualifiers, attributes into a single `VarAttr` + `Type *`. ~250 lines. |
| 960–1072 | **Declarator parsing** | `*`, `[]`, `(...)`, identifier — the recursive declarator grammar. |
| 1073–1254 | Array dimensions | `[N]`, `[*]` (VLA), `[]` (incomplete), `[CONST_EXPR]`. |
| 1255–1334 | Struct/union declarations | parsing `struct/union { ... }` bodies. |
| 1335–1395 | Struct member layout | size + offset + bitfield computations. |
| 1396–1412 | Union member layout | (simpler: max size, all offsets 0). |
| 1413–1429 | `__asm__("name")` labels on decls | for explicit symbol naming. |
| 1430–1691 | `__attribute__((..))` parsing | dispatch to per-attribute handlers (packed, aligned, section, used, weak, ...). |
| 1693–1759 | Enum parsing | `enum X { A, B = 5, C }`. |
| 1760–1804 | (untitled) misc declaration helpers. |
| 1806–1819 | Expression parsing entry | `expr` (top-level expression). |
| 1820–2284 | **Constant expression evaluation** | `eval_node` / `try_eval_node`; includes complex-expression evaluator (lines 2085–2284) for compile-time `_Complex` folding. ~460 lines. |
| 2285–2486 | Compound assignment + pointer arithmetic | `op=` lowering, `ptr +/- int` scaling. |
| 2487–2635 | Binary node construction with vector decomposition | wraps `new_binary` with vector-element-wise lowering. |
| 2636–2889 | Complex arithmetic helpers | `(a+bi)(c+di)` and `(a+bi)/(c+di)` lowering to scalar ops. |
| 2890–2908 | `find_member` | struct-member-by-name lookup. |
| 2909–4017 | **Expression grammar** (the bulk) | `assign` → `conditional` → `logor` → ... → `unary` → `postfix` → `primary` recursive descent.  Includes pointer arithmetic, member access, function calls, sizeof, alignof, generic, statement expressions, compound literals.  ~1100 lines. |
| 4019–4546 | **Statement parsing** | `compound_stmt` body, `if`, `while`, `for`, `do`, `switch`/`case`/`default`, `goto`/`label`, `break`/`continue`, `return`, expression statement.  ~530 lines. |
| 4547–4662 | Local declarations | inside functions, including auto-init lowering. |
| 4664–5176 | **Initializer parsing** | designated initializers, brace-elision, partial overrides, string-literal init, complex/vector init.  ~510 lines. |
| 5177–5352 | Init-code generation | converts the `Initializer` tree into a sequence of assignments. |
| 5353–5547 | **Global initializer evaluation** | compile-time evaluation of static / global init, including pointer-relative relocations. ~195 lines. |
| 5549–end | Top-level parsing | `parse` itself: walks token stream, dispatches to declaration/function-definition/typedef. |

The bold subsections (declaration specifiers, declarator parsing,
constant expression evaluation, expression grammar, statement
parsing, initializer parsing, global initializer evaluation) are
the natural Phase 4 spec sub-chunks.  Each is large enough to
warrant its own spec section.

---

## 3. Cross-cutting concerns

These concerns span multiple sections and need explicit treatment
in the spec.

### 3.1 Typedef handling

`Scope` tracks ordinary symbols, tags, and typedefs in separate
namespaces.  `is_typename` (§678) drives every place that needs to
distinguish "is this a type or an expression?" — sizeof,
expression-vs-declaration parsing, function-vs-variable
declarations, compound literal `(type){...}` vs cast `(type)expr`,
etc.  A subtle bug here cascades widely.

The spec must cover:
- When typedefs are looked up (every identifier in a context
  where a type is allowed).
- How typedef-of-typedef chains resolve (`copy_type` + `origin` —
  shared with type system §6).
- The lookahead pattern: `is_typename(tok)` peeks without
  consuming.

### 3.2 Constant expression evaluation

`eval_node` / `try_eval_node` walk Node trees and fold to
`int64_t`.  Used by:
- Array dimensions (`int a[N]` where N must be a constant).
- Bitfield widths.
- Enum values.
- Static initializers.
- `sizeof` / `alignof` / `_Static_assert`.

The two-mode design (fatal `eval_node` vs falsey `try_eval_node`)
is load-bearing: many callers need "if it's a constant, use it;
otherwise emit code at runtime."

The complex-number evaluator (§2085–2284) is a separate
specialization that runs inside `eval_node` when the operand is a
`_Complex` expression.

### 3.3 Complex (`_Complex`) lowering

`_Complex` types are first-class in ncc per the type system (§4).
The parser lowers complex arithmetic (`a + bi`, `(a+bi)*(c+di)`,
etc.) into scalar real/imaginary operations at parse time — there
is no runtime complex arithmetic; codegen only sees scalar
operations on a tmp variable that has `__real__` and `__imag__`
fields.

The lowering pattern is consistent: build a tmp local of type
`_Complex T`, assign real and imaginary parts via `__real__` /
`__imag__` member access, and use the tmp in subsequent
operations.

### 3.4 Vector (`__attribute__((vector_size))`) lowering

Similar pattern to complex: vector arithmetic is decomposed into
element-wise scalar operations at parse time (`new_binary_vector`
in §2487).  The codegen does not have native vector instructions;
it sees a sequence of scalar operations on the underlying array.

This is a known performance simplification — real SIMD codegen is
out of Phase 4 scope.

### 3.5 VLA handling

`Type *` with `kind == TY_VLA` carries a Node-typed `vla_len`
(runtime expression).  The parser:
- Detects VLA in declarator parsing (§1073: `[*]` and
  `[non_constant_expr]`).
- Generates a hidden local var (`__vla_size_N`) to hold the
  byte size at runtime (§491).
- Inserts the size-computation statement into the surrounding
  block.
- VLA-of-pointer decay deferred to `new_add`/`new_sub` per
  type-system §9.5 (Q6).

The spec must capture the VLA-byte-size codegen pattern
explicitly — it is not obvious from the C standard.

### 3.6 Scope management

Three namespaces per scope:
- **Tag** (struct/union/enum names).
- **Ordinary** (variables, functions, typedef names, enum
  constants).
- **Labels** (per-function, separate scope chain).

`enter_scope` / `leave_scope` push/pop on a linked list.  Lookups
walk inward-to-outward.  Function-body-local scopes are pushed
inside the function definition; struct/union member scopes are
pushed when parsing the body.

### 3.7 Nested functions (GCC extension)

`src/parse.c:51–72` documents nested-function support: a function
defined inside another function captures outer variables.
Implemented via trampolines (forbidden on macOS due to W^X — most
of the related torture tests are SKIP'd).  The parser emits the
support code; codegen has the W^X restriction.

The spec should describe nested-function semantics for
completeness (so the rewrite preserves them), but the code path
is exercised mainly by the gcc torture suite, not by real
programs.

### 3.8 Designated initializers

Initializer parsing (§4664) handles all of:
- Brace-elision: `int a[2][2] = {1, 2, 3, 4}` (no inner braces).
- Designated init: `struct S s = { .a = 1, .b = 2 }`.
- Array index init: `int a[] = { [3] = 5 }`.
- Partial overrides: later designators overwrite earlier
  designators that target the same slot.
- String-literal init for char arrays.
- Compound literals as initializer values.
- Complex / vector initializers.

This is one of the most subtle parts of the C standard.  The
spec must enumerate the brace-elision rules carefully —
miscounted braces produce wildly wrong layouts.

---

## 4. Data shapes the parser produces

The parser builds:

- **`Obj`** — a function or global variable.  `parse` returns the
  head of an `Obj` list.
- **`Node`** — AST node (~50 distinct kinds, see `enum NodeKind`
  in `cc.h`).
- **`Type`** — built via type system factories (Phase 3) but
  populated with declarator-specific info here.
- **`Member`** — struct/union member descriptor (offset,
  bit-width, embedded-struct chain for anonymous structs).
- **`Scope`** — internal-only; never returned.
- **`Initializer`** — a tree mirroring the structure of an init,
  consumed by `create_lvar_init` to produce assignment statements.

The `Obj` linked list and the `Node` graph are what codegen
consumes.

---

## 5. Recommended Phase 4 spec subdivisions

Following the size of each subsection, the eventual `04_parse.md`
spec should have ~12–15 sections, broadly:

1. **Scope** (§6 of inventory): scope chains, lookup, namespace
   separation.
2. **Data model**: `Scope`, `Obj`, `Initializer`, `Member`,
   helpers; references to `Type` and `Node` defined elsewhere.
3. **Top-level entry**: `parse`.
4. **Declaration specifiers**: storage class, type-spec combiner,
   qualifiers, attributes.
5. **Declarator**: `*`, `[]`, `(...)`, identifier — recursive
   syntax.
6. **Array / VLA**: dimensioning and runtime-size-codegen pattern.
7. **Struct / union / enum**: bodies, member layout, bitfield
   layout.
8. **Expression grammar**: precedence ladder, primary forms,
   compound literals, sizeof/alignof, _Generic, statement
   expressions.
9. **Constant expression evaluation**: `eval_node` /
   `try_eval_node`, including complex-expression folding.
10. **Statement parsing**: control flow, labels, goto, switch.
11. **Initializers**: designated, brace-elision, partial
    overrides, string-literal init.
12. **Function definitions**: parameter binding, body, label
    scope, nested functions.
13. **Global initializer evaluation**: compile-time fold +
    pointer-relative relocations.
14. **Known divergences**: GCC extensions accepted, C standard
    behaviors omitted.
15. **Validation criteria**: bootstrap fixed-point, torture,
    real programs, regression tests.
16. **Out of scope**: lexing (Phase 1), preprocessing (Phase 2),
    type construction (Phase 3), codegen (Phase 5).

Phase 4's spec will be the longest of the swap-out — likely
2500–3500 lines.

---

## 6. GCC/clang extensions in `parse.c`

Per a grep over the file, parse.c uses few language extensions
in its own source — the mass of `__attribute__` handling is
parser logic for *user code*, not parser implementation.

Implementation language extensions (to be replaced for pure C11):
- `= {0}` is used (standard C); `= {}` empty initializer (GCC
  extension) appears in a handful of struct-init sites.  Will
  need replacing.
- POSIX `strdup` / `strndup` / `format` (project helper) — same
  story as preprocess.c; replace via `strndup_checked` from
  `alloc.c` plus a few inline helpers.

The user-facing extensions parse.c **accepts** are part of the
language ncc supports and stay as-is:
- `__attribute__((...))` (parsed and dispatched to handlers).
- `__typeof__` / `__typeof` (evaluated as type-of operator).
- Statement expressions `({ ... })`.
- Labels-as-values `&&label`.
- Nested functions (with trampolines, OS-permitting).
- Compound literals in initializers.
- Vector types (`vector_size`).
- Complex types (C11 `_Complex`).
- `__sync_*` / `__atomic_*` builtins (parsed; codegen lowering is
  Phase 5 scope).

---

## Notes for the spec author

Gotchas, non-obvious invariants, and places where a test case
would be valuable.  This list will grow as spec drafting
discovers more edge cases.

- **`is_typename` is the watershed.** Every "should I parse this
  as a type or an expression?" branch flows through it.  A
  correctness bug here corrupts everything downstream.  Spec must
  enumerate the conditions exhaustively.

- **Complex / vector lowering happens at parse time.** Codegen
  sees only scalar operations; the parser is responsible for
  emitting `(__real__ tmp = R, __imag__ tmp = I, tmp)` patterns
  and similar for vectors.  Phase 5 codegen audit will not need
  to re-implement this lowering — but the parse-time pattern is
  load-bearing and must survive the rewrite intact.

- **VLA size evaluation has side-effecting expressions.** `int
  a[f()]` evaluates `f()` at the declaration's runtime; the
  parser generates a hidden local + assignment statement that
  must be inserted before the declaration's first use.  Spec
  must describe the insertion point.

- **Designated-init brace-elision is subtle.**  The C standard
  describes this with intricate precedence rules; chibicc's
  implementation has been corpus-tested and ought to be
  preserved exactly.  A worked example test case for partial
  overrides + brace-elision combined is high-value.

- **Constant expression evaluation overlaps codegen folding.**
  parse.c's `eval_node` is for parse-time constant folding;
  codegen has its own folding pass.  Keep the parse-time fold
  conservative (don't try to be the optimizer); the codegen pass
  is what matters for real-world performance.

- **`_Static_assert` short-circuits via try_eval_node.**  The
  parser uses `try_eval_node` to attempt compile-time evaluation
  and emits the assertion message only if it fails to fold to
  non-zero.  Spec must define when an expression is "constant
  enough" for `_Static_assert`.

- **String-literal initialization of char arrays.**  `char s[] =
  "hello"` produces `s` of size 6 (5 chars + NUL).  This is a
  parser-only behavior — codegen sees a normal byte-array init.

- **Forward-declared static functions in file-scope initializers.**
  Per `docs/swap-out-log.md` divergence-log entry for `ff529fb`,
  this is a known main-branch fix that has not yet been ported to
  swap-out (gated on Phase 4).  The Phase 4 reimpl should match
  main's behavior and the relevant compliance test should land
  alongside.

- **Nested functions on macOS.**  Trampoline-based nested
  functions don't work on macOS due to W^X restrictions on
  executable stack regions.  ncc parses them and codegen emits
  them, but they fault at runtime.  Most torture tests that
  exercise nested functions are SKIP'd via `dg-require-effective-
  target trampolines`.  Phase 4 should preserve the parse-side
  support; runtime behavior is Phase 5's problem.

- **Labels-as-values address production.**  `&&label` produces a
  pointer-to-void.  Used by `goto *expr`.  Both parsed in
  parse.c.  Codegen materialization is Phase 5 scope.

- **Function definitions vs declarations.**  The parser
  distinguishes via the next token after the declarator: `{`
  starts a function definition; `;` (or `,`-separated decls)
  ends a declaration.  Subtle for K&R-style declarations (not
  in scope for this corpus).

- **`__attribute__((cleanup(fn)))` is parsed but not lowered.**
  parse.c accepts the syntax; codegen has no cleanup support.
  The corpus does not exercise this; behavior is "parse and
  ignore."  Spec should call out which attributes are parsed-
  and-dispatched vs parsed-and-ignored.

- **The Phase 1/2/3 divergence-log backlog applies here.**
  Several main-branch fixes are gated on Phase 4 (per
  `docs/swap-out-log.md`):
  - `150f17d` parse: `try_eval_node` FP fix (gated on `93c6ecc`'s
    if-fold caller).
  - `e7e7393` codegen: variadic va_start fix (Phase 5 scope, but
    Phase 4 must surface the trigger condition).
  - `ff529fb` parse: forward-static-fn fix (Phase 4 directly).
  - `93c6ecc` parse + codegen: NetBSD bundle (Phase 4/5 jointly).
  These should be ported to swap-out as part of Phase 4 spec
  authoring + impl, with `tests/compliance/` repros landing
  alongside.

- **The parse.c → codegen interface is the AST.**  Phase 4's
  reimplementation must produce identical Node graphs (modulo
  trivial differences like temp-variable names).  The cleanest
  validation is bootstrap fixed-point — if ncc-v2 (Phase 4
  candidate) produces the same compiler when self-hosting, the
  AST shapes match.
