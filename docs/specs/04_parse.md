# ncc Parser — English Specification (Phase 4, top-level)

This document is the **cross-cutting contract surface** for ncc's
parser on the `swap-out` branch.  Phase 4 of the chibicc swap-out
replaces the inherited `src/parse.c` (6136 lines, chibicc lineage)
with a reimplementation written from this spec.  Both the current
code and the reimplementation must satisfy the contract;
mechanical validation (`scripts/bootstrap_validate.sh` plus the
test corpus) verifies behavioral equivalence.

This is not a from-scratch C parser design.  The goal is
byte-for-byte behavioral equivalence with `main`'s parser, achieved
through a spec-mediated rewrite that removes chibicc-lineage code
from `src/`.

When the implementation and this document disagree, the document
is authoritative; bugs in the implementation are fixed against the
document.  When the document and `main`'s observable behavior
disagree, this document is wrong and must be updated, unless the
divergence is recorded in §13 as intentional.

**Phase 4 scope:** the parser module (`src/parse.c`).  Tokenization
is owned by Phase 1, preprocessing by Phase 2, type construction
by Phase 3, codegen by Phase 5.

**Document family:** Phase 4's spec is split per Q1.B
(`04_parse_questions.md`) into one top-level (this document) plus
four sub-chunks.  Each sub-chunk is a standalone behavioral spec
for its zone of `parse.c`; this document covers concerns that span
all of them.

| File | Zone of `parse.c` | Approximate lines |
|---|---|---|
| `04_parse.md` (this doc) | Cross-cutting contract surface | ~700 |
| `04a_decl.md` | Declaration specifiers, declarators, struct/union/enum, array/VLA, attributes, top-level entry | ~1300 |
| `04b_expr.md` | Expression grammar, constant expression evaluation, complex/vector lowering | ~1400 |
| `04c_stmt.md` | Statement parsing, function definitions, local declarations, scope mechanics in practice | ~700 |
| `04d_init.md` | Initializers, designated init, brace elision, init-code generation, global initializer evaluation | ~900 |

Sub-chunks cite this document by section number when invoking a
cross-cutting decision (e.g., "per §4 of `04_parse.md`,
`is_typename` returns true here").

**Current status (2026-05-04):** Phase 4 baseline tagged at
`phase-4-baseline` (commit `542d534`).  Inventory and design-Q&A
documents resolved.  Spec drafting in progress per Q2.C: top-level
+ `04a_decl.md` first, then sub-chunks in any order.

---

## 1. Scope

The parser provides one job: consume a preprocessed token stream
and produce an AST that codegen can lower to assembly.

**Input:** a `Token *` linked list, post-preprocessing.  Each
`Token` already has its kind (`TK_IDENT`, `TK_NUM`, `TK_STR`,
`TK_KEYWORD`, `TK_PUNCT`, `TK_EOF`), source-position metadata, and
type/value fields populated.

**Output:** a linked list of `Obj *` records.  Each `Obj` is either
a function (with a populated `body` AST and parameter/local lists)
or a global variable (with optional `init_data` + relocation
info).  Codegen consumes this list directly.

**Side effects, by design:**
1. Constructed `Type *` and `Node *` graphs allocated via the
   project allocator (`alloc.c`) — never freed during compilation.
2. Mutation of file-scope state: the running `Obj` chain, the
   anonymous-name counter (`new_anon_gvar`), the unique-label
   counter (§109–113 of `src/parse.c`).
3. Insertion of compiler-generated AST nodes for VLA size
   computation (§7), complex/vector lowering (§6), and initializer
   lowering (`04d_init.md`).

**Side effects out of scope:** I/O, diagnostics-format choice
(error formatting is shared infrastructure in `error.c`),
header-search policy (Phase 2).

---

## 2. Public surface

The parser's externally-visible contract is four functions
declared in `cc.h`:

| Function | Caller | Purpose |
|---|---|---|
| `Obj *parse(Token *tok)` | `main.c` | Top-level entry. Returns the head of an `Obj` list. |
| `Node *new_cast(Node *expr, Type *ty)` | `type.c` (`usual_arith_conv`), parser-internal | Wrap an expression in a typed cast. Sets `Node->ty`. |
| `int64_t eval_node(Node *node)` | parser-internal, called from `cc.h`-visible callers | Fold a node to `int64_t`. Fatal on non-constant operand. |
| `bool try_eval_node(Node *node, int64_t *out)` | `_Static_assert` lowering, init code | Non-fatal evaluation; returns `false` if non-constant. |

Everything else in `parse.c` is `static`.  The reimplementation may
freely refactor internals; the four-function surface is the durable
contract.

The full specification of `eval_node` / `try_eval_node` lives in
`04b_expr.md` §K (constant expression evaluation).  This file
mentions them only for cross-references.

---

## 3. AST shape contract (Q4)

**Decision (Q4.A):** the reimplemented parser must produce
**Node-shape preservation** with respect to `main`'s parser.  That
is, for every accepted input, the v2 parser produces a Node tree
such that codegen emits **byte-identical** assembly.

Operationally, this means same `NodeKind`, same children in the
same slots, same `Type *` pointers (where the type system §3
singletons are involved) or structurally-equal `Type *` (otherwise),
same line/column metadata, same generated names.

**Permitted differences (the only known ones):**
- `new_anon_gvar`'s anonymous-symbol counter is process-wide.
  When v1 and v2 both run in the same compiler invocation (e.g.
  during stage-1 build where v1 is canonical), counter values
  diverge by construction.  For self-host bootstrap (where v2 is
  canonical and v1 is gone), each run is internally consistent
  and counter values match across stages.  This is the relevant
  invariant for the fixed-point gate.
- Temporary local variable names (`__tmp_N`, `__vla_size_N`,
  etc.) follow the same counter rule.

**Validation:** the closure gate is
`scripts/bootstrap_validate.sh` exit 0 plus full test corpus pass
(Q9.A).  Bootstrap fixed-point requires byte-identical compiler
output across two stages of self-hosting; in practice this means
identical-enough AST for codegen.  No AST-dumping mode is built
for Phase 4 (Q9 alternative B is deferred).

---

## 4. Scope and namespaces

### 4.1 Three namespaces per scope

Each lexical scope tracks three independent symbol namespaces:

1. **Tag namespace** — names of struct/union/enum types
   (`struct Foo`, `enum E`).  Looked up by `find_tag`.
2. **Ordinary namespace** — variables, functions, typedef names,
   enum constants.  Looked up by `find_var`.
3. **Label namespace** — per-function, separate from the lexical
   scope chain.  Labels are forward-resolvable within their
   function.

A typedef name `T` and a variable named `T` may coexist:

```c
typedef int T;
int T = 5;       // ordinary namespace; shadows nothing
int x = sizeof(T);   // 4 — typedef
int y = T;            // 5 — variable
```

(See `tests/regression/NN_typedef_shadow.c` once authored —
deferred per Q14.A; the existing torture covers the common cases.)

### 4.2 Scope chain

`Scope` records form a singly-linked chain.  `enter_scope` pushes
a new record; `leave_scope` pops it.  Lookup walks inward to
outward.  The chain is per-translation-unit; nested function
definitions push additional scopes (§9).

`Scope` is parser-internal; it is never exposed via the public
surface.  Detailed structure lives in `04c_stmt.md` §A.

### 4.3 Where scopes are pushed

| Construct | Scope pushed on | Scope popped on |
|---|---|---|
| Function definition | `{` of body (after parameter binding) | `}` |
| Block statement | `{` | `}` |
| `for (decl; ...; ...) ...` | the `for` keyword | end of body |
| `struct/union { ... }` body | `{` | `}` (members are in their own namespace inside the tag, not the enclosing scope) |
| Statement expression `({ ... })` | inner `{` | inner `}` |

`if`, `while`, `do`, `switch` do **not** push a scope around the
controlled statement — only around `{ ... }` blocks.  This matches
C11 §6.8.4/3 (selection statements introduce no scope; their
controlled statement is its own block if braced).

---

## 5. Typedef handling and `is_typename` (Q5)

### 5.1 Why `is_typename` is the watershed

Every parsing decision of the form "is this token sequence a
type or an expression?" routes through `is_typename(Token *tok)`:

- `sizeof X` vs `sizeof(T)`.
- `(T)x` cast vs `(expr)` parenthesized expression.
- `(T){...}` compound literal.
- Statement: declaration vs expression statement.
- Function parameter: `f(T)` declaration vs `f(x)` call argument.

A misclassification corrupts everything downstream.  The function
**must not consume** the token; it inspects only.

### 5.2 Exhaustive keyword set (Q5.A)

`is_typename` returns `true` if the token's spelling matches any
keyword in the table below, **or** if the token is an identifier
that resolves via the ordinary-namespace scope chain to a typedef
binding (`VarScope.type_def != NULL`).

The table below is the complete normative list — 35 keywords
total, taken verbatim from `src/parse.c` lines 682–690 on `main`
at `phase-4-baseline`.  Adding or removing a keyword changes the
language.

| Group | Keywords |
|---|---|
| Primitive types | `void`, `_Bool`, `char`, `short`, `int`, `long`, `float`, `double` |
| Signedness / complex | `signed`, `unsigned`, `_Complex`, `__complex__` |
| Tag specifiers | `struct`, `union`, `enum` |
| Storage class | `typedef`, `static`, `extern`, `auto`, `register`, `_Thread_local`, `__thread` |
| Function specifiers | `inline`, `_Noreturn` |
| Qualifiers | `const`, `volatile`, `restrict`, `_Atomic` |
| Alignment specifier | `_Alignas` |
| `typeof` family | `typeof`, `__typeof`, `__typeof__` |
| GCC misc | `__extension__`, `__builtin_va_list`, `__attribute__` |

**Notes:**
- The bare-`typeof` form is a GCC extension, not a C11 keyword;
  `is_typename` accepts it for source-compatibility with GCC
  headers.
- `_Alignas` triggers a special `_Alignas(N)` / `_Alignas(T)`
  parse path inside `declspec`; it is not the operator
  `_Alignof`.  `_Alignof` lives in unary expression parsing
  (`04b_expr.md` §G.1) and is **not** in `is_typename`.
- `__restrict` and `__restrict__` are **not** in `is_typename`.
  They are accepted as pointer qualifiers inside `pointers()`
  (`04a_decl.md` §C) but cannot start a declaration on their own.
  This matches GCC's behavior: `int *__restrict p;` is valid;
  `__restrict int *p;` is not.
- `__signed`, `__signed__`, `__unsigned`, `__const`, `__volatile`,
  `__volatile__`, `__inline`, `__inline__` are **not** accepted.
  Sources that need them rely on system headers `#define`-ing
  them to the bare form.
- `_Alignof`, `__alignof`, `__alignof__` are operator-position-only
  and not in `is_typename`.

**Identifier resolution:** for any token of kind `TK_IDENT`,
`is_typename` calls `find_var(tok)` and returns `true` iff the
resulting `VarScope` is non-NULL **and** has a non-NULL
`type_def` field.  Lookup walks the scope chain inward to
outward.  An identifier that is bound as both a variable and a
typedef in different enclosing scopes resolves to the innermost
binding.

The keyword check is implemented as a linear search over a
`static const char *kw[]` array (parse.c current line 682); the
reimplementation may use the same shape or a perfect-hash table
generated at build time.

### 5.3 Typedef chain resolution

A typedef may reference another typedef:

```c
typedef int A;
typedef A B;
typedef B C;
```

Per type-system §6 (`copy_type` + `origin`), all derived types
share a single canonical Type pointer reachable via the `origin`
chain.  The parser does not flatten chains itself; it constructs
the typedef's RHS once via `declspec` (`04a_decl.md` §B), stores
the resulting Type in the ordinary namespace, and downstream
lookups receive that Type directly.

`find_typedef(tok)` returns the stored `Type *` (or NULL).
Callers that need the canonical (chibicc-flattened) form invoke
type-system helpers — not the parser's responsibility.

### 5.4 Lookahead invariants

The parser uses one-token lookahead almost everywhere.  Two
exceptions:

1. **Declaration vs expression statement.** `is_typename(tok)`
   inspects `tok` (no lookahead beyond `tok`); the result decides
   the parse path.
2. **`(type){...}` compound literal vs `(type)expr` cast vs
   `(expr)` parenthesized expression.** When the parser sees `(`,
   it peeks the next token (`tok->next`) and tests
   `is_typename(tok->next)`; this is two-token lookahead.  See
   `04b_expr.md` §F.

No third lookahead position is ever needed.  The reimplementation
must preserve the one-token-default rule.

---

## 6. Constant expression evaluation (Q6)

### 6.1 Two entry points, distinct contracts

`int64_t eval_node(Node *node)`:
- Walks `node` and returns the folded `int64_t`.
- **Fatal** on non-constant operand: emits a diagnostic and
  terminates compilation.
- Used at points where the C standard requires a constant
  expression (array dimensions in non-VLA context, bitfield
  widths, enum values, static initializers, `_Static_assert`'s
  failure-message-suppression case).

`bool try_eval_node(Node *node, int64_t *out)`:
- Walks `node`; if foldable, writes the result via `out` and
  returns `true`; else returns `false`.
- **Non-fatal**.
- Used at points where a constant fold is desired but not
  required (`_Static_assert` short-circuit, init-code generation
  to choose between compile-time and runtime store).

The two-mode design is load-bearing: many callers need "if it's
a constant, use it; otherwise emit code at runtime."  Replacing
either with a wrapper around the other (e.g., catching the fatal
path) breaks the contract.

### 6.2 Fold scope (Q6.A — preserved exactly)

The folder evaluates the following node kinds.  Every kind not
listed below causes `try_eval_node` to return `false` and
`eval_node` to fail with diagnostic:

**Numeric leaves:** `ND_NUM`.  Returns the stored value.

**Unary arithmetic:** `ND_NEG`, `ND_BITNOT`, `ND_NOT`, `ND_CAST`
(integer-to-integer; integer-to-float and float-to-integer follow
the C semantics in `04b_expr.md` §K), `ND_LOGAND` / `ND_LOGOR`
short-circuit form.

**Binary arithmetic:** `ND_ADD`, `ND_SUB`, `ND_MUL`, `ND_DIV`,
`ND_MOD`, `ND_BITAND`, `ND_BITOR`, `ND_BITXOR`, `ND_SHL`, `ND_SHR`,
`ND_EQ`, `ND_NE`, `ND_LT`, `ND_LE`.

**Conditional:** `ND_COND` (the `?:` operator).  Unfolded branch
is **not** evaluated (preserves short-circuit).

**Pointer-relative:** `ND_VAR` for static-storage objects yielding
their address; arithmetic on such addresses produces relocation
records (handled in `04d_init.md` §G, not by `eval_node`).

**Float folding:** when the result type is floating, an internal
`eval_double(Node *)` is used; the public `eval_node` returns the
bitwise-cast `int64_t`.  Float fold scope mirrors integer fold
scope where the operator is defined for floats.

**Complex folding:** `_Complex T` arithmetic is folded by a
specialized walker (`eval_complex`, `parse.c` lines 2085–2284).
Scope: same operators as scalar, applied per real/imag component.
Detailed in `04b_expr.md` §K.4.

**Out-of-scope (always non-constant):** function calls, variable
loads from non-static storage, dereferences, member accesses on
runtime values, statement expressions, `_Generic` selection (the
*selected* branch is folded; selection itself is parse-time).

### 6.3 Why parse-time folding exists when codegen also folds

Parse-time folding services constructs that **must** be
evaluable at parse time (array dimensions, enum values, etc.)
because the resulting integer feeds back into the AST shape.
Codegen-time folding services performance.  Preserving exactly
the same parse-time scope avoids the failure mode where parse-fold
and codegen-fold disagree on overflow semantics for the same
operator.

---

## 7. VLA model (Q8)

### 7.1 Detection

A variable-length array is any `[expr]` declarator whose `expr`
is not a constant expression per §6, **or** the explicit `[*]`
form.  Detection happens in declarator parsing (`04a_decl.md`
§D).

`int a[10]` — fixed array (`TY_ARRAY`).
`int a[N]` where `N` is `enum { N = 10 }` — fixed array.
`int a[n]` where `n` is a runtime variable — VLA (`TY_VLA`).
`int a[*]` — VLA in a function prototype, length unspecified.
`int a[]` at non-prototype scope — incomplete array
(`array_len = -1`), not a VLA.

### 7.2 Hidden-local size variable

For each VLA declarator, the parser emits a hidden local of type
`size_t` named `__vla_size_N` (N = unique counter).  Its value is
the byte size of the array, computed at runtime.

For `int a[f()]`, the emission is equivalent to:

```c
size_t __vla_size_1 = (size_t)f() * sizeof(int);
int (*a)[__vla_size_1 / sizeof(int)] = alloca(__vla_size_1);
```

(The actual AST is more direct — the parser does not emit C
source; this is the Node-tree equivalent.)

### 7.3 Insertion point (Q8.A)

The size-computation Node is inserted into the **enclosing
block's statement list at the position of the declaration**, not
at the top of the block.  This means side effects in the size
expression run at the point the declaration is reached, not
earlier:

```c
{
  printf("a\n");                  // 1: prints "a"
  int a[f()];                     // 2: f() runs here
  printf("b\n");                  // 3: prints "b"
}
```

If `f()` were hoisted to the top of the block, output would be
`"a"`-prefixed by `f()`'s side effects — wrong.

The mechanism: `compound_stmt` in `04c_stmt.md` §B builds the
statement list incrementally; when a declaration containing a VLA
is parsed, the parser appends the size-computation statement to
the list **before** appending the declaration's own
initialization statement(s).  The two appends happen back to
back, so insertion-point ordering is locally correct without a
buffering mechanism.

### 7.4 VLA-of-pointer decay

Per type-system §9.5 (Q6 of `03_type_questions.md`), a VLA used
in pointer context decays to a pointer-to-element, *not* a
pointer-to-VLA.  The parser does not handle this directly; it is
deferred to `new_add` / `new_sub` (`04b_expr.md` §J) which
inspects the operand type.

### 7.5 Worked example

```c
void f(int n) {
  int x = 0;
  int a[n];      // VLA
  a[0] = 1;
  x = sizeof(a); // n * sizeof(int)
}
```

Parser output (Node tree, schematic):

```
ND_BLOCK
├─ ND_VAR(x) = ND_NUM(0)                  // x declaration
├─ ND_VAR(__vla_size_1) =                 // hidden local; inserted
│    ND_MUL(ND_CAST(size_t, ND_VAR(n)),    //   at point of `int a[n]`
│           ND_NUM(sizeof(int)))
├─ ND_VAR(a) = alloca(ND_VAR(__vla_size_1))   // VLA decl proper
├─ ND_ASSIGN(ND_DEREF(ND_ADD(a, 0)), 1)
└─ ND_ASSIGN(x, ND_VAR(__vla_size_1))     // sizeof(a) → __vla_size_1
```

The crucial invariant: the `__vla_size_1` declaration appears
between `x = 0` and the use of `a`, not at block top.

(Test: `tests/regression/NN_vla_side_effect.c` per Q14.C.)

---

## 8. Complex and vector parse-time lowering (Q7)

### 8.1 Decision (Q7.A): preserved

`_Complex T` and `__attribute__((vector_size(N)))` arithmetic is
decomposed into scalar real/imag (complex) or element-wise
(vector) operations **at parse time**.  Codegen sees no complex
multiply, no vector add — only scalar ops on a tmp variable.

This is the established contract.  Phase 5 may revisit (real
SIMD codegen would benefit), but Phase 4's job is preservation.

### 8.2 Complex lowering shape

For `_Complex T tmp = (a + bi) * (c + di)`:

The parser:
1. Constructs a tmp local `__tmp_complex_N` of type `_Complex T`.
2. Computes the real component `a*c - b*d` as a scalar `T`
   expression and assigns it to `__real__ __tmp_complex_N`.
3. Computes the imaginary component `a*d + b*c` and assigns it
   to `__imag__ __tmp_complex_N`.
4. Substitutes `__tmp_complex_N` for the original expression.

`__real__` and `__imag__` are pseudo-member accesses on the
underlying struct-like layout (real is offset 0, imaginary is
offset `sizeof(T)`).  See `04b_expr.md` §H.

### 8.3 Vector lowering shape

For `vec_t a = b + c` where `vec_t` is `__attribute__((vector_size(16)))
int`:

The parser:
1. Constructs a tmp local `__tmp_vec_N` of type `vec_t`.
2. For each element index `i` in `0..N-1`, emits a scalar
   addition of `b[i] + c[i]` and assigns to `__tmp_vec_N[i]`.
3. Substitutes `__tmp_vec_N` for the result.

The emitted Node tree is roughly N copies of the scalar form,
sequenced by a comma operator.  For 16-byte / 4-element vectors,
the expansion is 4 scalar ops; for 64-byte vectors, 16 ops.

### 8.4 Why parse-time, not codegen-time

Two reasons.  First, the type system already represents vectors
and complex via existing shapes (vector as array-with-flag,
complex as 2-tuple) — codegen has no special knowledge of either,
and adding it would require new `ND_*` kinds.  Second, the
existing approach is corpus-tested (the gcc torture suite
exercises both extensively).

The cost is poor codegen for SIMD-shaped programs.  Phase 5 may
re-raise this, but until then, parse-time decomposition is the
contract.

---

## 9. Nested functions (Q10)

### 9.1 Decision (Q10.A): parse path preserved

GCC nested-function syntax is parsed.  Codegen emits trampolines.
On macOS, trampolines fault at runtime due to W^X; on Linux they
work.

The parser recognizes a function definition inside another
function's body as a nested function, captures outer variables
referenced from the inner body, and produces an `Obj` with a
populated `outer_vars` list.  Codegen consumes `outer_vars` to
generate trampoline code.

This is a ~50-line parser feature.  Removing it (Q10.B) would
break source code that uses the GCC extension on Linux without
saving meaningful complexity.

### 9.2 Captured-variable model

A reference inside the inner function to a name defined in an
enclosing function's scope captures the outer variable.  The
captured variable is recorded in the inner function's
`outer_vars` list; codegen materializes a closure-like structure.

The parser's job is identification and recording — not
trampoline emission.  The trampoline mechanism is codegen's
domain (Phase 5).

### 9.3 Out of scope here

Trampoline runtime correctness is Phase 5.  This document
describes only the parse-side capture model so the rewrite
preserves it.  Detail in `04c_stmt.md` §F (function definitions).

---

## 10. Designated initializer reference (Q11)

### 10.1 Decision (Q11.A): C11 cite + worked examples

Designated initializers, brace elision, and partial overrides
are specified by **reference to C11 §6.7.9** plus worked examples
in `04d_init.md` §C–§E.  The chibicc implementation has been
corpus-tested against the GCC torture suite for years; that
ground-truth equivalence is the operational contract.

The full algorithmic description lives in `04d_init.md`.  This
document mentions the topic only as one of the cross-cutting
gotchas the spec author should treat with care.

### 10.2 Why a worked-example approach

Pure prose translation of the C11 algorithm rots; the standard
itself is intricate, and a translation introduces new failure
modes in the prose.  Cite-the-standard plus diagnostic-quality
examples lets a reviewer check the spec against ground truth (the
test corpus) without re-deriving the standard.

---

## 11. Attribute disposition table (Q12)

### 11.1 Decision (Q12.A): explicit table

The parser handles a finite set of `__attribute__((...))` forms.
Each is classified as **HONORED** (lowered by codegen),
**PARSED-AND-IGNORED** (accepted, no semantic effect), or
**REJECTED** (compile-time error).

| Attribute | Disposition | Notes |
|---|---|---|
| `aligned(N)` | HONORED | Object alignment override. |
| `packed` | HONORED | Disable struct member padding. |
| `section("name")` | HONORED | Place object in named section. |
| `weak` | HONORED | Weak linkage. |
| `used` | HONORED | Suppress unused-symbol GC. |
| `noreturn` | HONORED | Equivalent to `_Noreturn`. |
| `unavailable` | HONORED | Diagnostic-only; reject if referenced. |
| `vector_size(N)` | HONORED | Vector type construction (§8.3). |
| `mode(M)` | HONORED | Type-mode override (`mode(QI)` → 8-bit, etc.). |
| `format(...)` | PARSED-AND-IGNORED | Printf-style checking; ncc has no warning pass. |
| `nonnull(...)` | PARSED-AND-IGNORED | Likewise. |
| `pure`, `const` (fn) | PARSED-AND-IGNORED | Optimizer hints; ncc has no optimizer. |
| `nothrow` | PARSED-AND-IGNORED | C++ artifact in C headers. |
| `cleanup(fn)` | PARSED-AND-IGNORED | Syntax accepted; codegen does not lower. |
| `deprecated[(msg)]` | PARSED-AND-IGNORED | No diagnostic emitted. |
| `visibility("...")` | PARSED-AND-IGNORED | Default visibility on macOS. |
| `always_inline`, `noinline` | PARSED-AND-IGNORED | ncc does not inline. |
| `gnu_inline`, `artificial` | PARSED-AND-IGNORED | GCC inline-spec artifacts. |
| `warn_unused_result` | PARSED-AND-IGNORED | No warning pass. |
| `malloc`, `alloc_size`, `alloc_align` | PARSED-AND-IGNORED | Optimizer hints. |
| `returns_twice` | PARSED-AND-IGNORED | `setjmp`-class; codegen treats normally. |
| `may_alias` | PARSED-AND-IGNORED | TBAA hint; ncc does no aliasing analysis. |
| `transparent_union` | REJECTED | Not implemented; would need union-passing changes. |
| `no_sanitize(...)` | PARSED-AND-IGNORED | No sanitizer pass. |
| `unknown attribute` | PARSED-AND-IGNORED with warning suppressed | GCC convention; permits forward compatibility. |

The "unknown attribute" fallback is significant: the parser
accepts any `__attribute__((name(...)))` form whose `name` is not
in the rejected set, recording but not honoring.  This permits
real-world headers (libc, system headers) to compile despite
referencing GCC attributes ncc doesn't implement.

(Test: `tests/regression/NN_attr_table.c` per Q14.F — probes
each HONORED for observable codegen change and each
PARSED-AND-IGNORED for silent acceptance.)

### 11.2 Where attributes attach

Attributes may attach to:
- A declaration (variable, typedef, function).
- A type (struct, union, enum tag).
- A struct/union member.
- A function parameter.

Attribute-parsing is dispatched from declarator parsing
(`04a_decl.md` §G).  The set of honored attributes is identical
across attachment points (an `aligned(N)` on a struct member
honors per-member alignment; on a variable, per-variable; etc.)

---

## 12. Forward-declared static fn fix (Q13)

`ff529fb` on `main` fixed a parser bug where a forward-declared
static function referenced in a file-scope initializer was
rejected.  The fix permits:

```c
static int g(int);
int (*p)(int) = g;       // OK after fix; rejected before.
static int g(int x) { return x + 1; }
```

**Decision (Q13.A):** Phase 4's spec includes the fixed behavior
from the start.  The reimplementation matches `main` post-`ff529fb`.
A new compliance test (`tests/compliance/forward_static_fn.c` or
similar) lands alongside the Phase 4 swap-in.

The mechanism: in file-scope initializer evaluation
(`04d_init.md` §G), references to functions resolve via the
ordinary-namespace lookup at scope time, but a forward
declaration is sufficient for the reference to be recorded; the
definition is matched at end-of-translation-unit.

---

## 13. Known divergences from `main`

This section will accrete entries during Phase 4 implementation.
Initial state: empty.  Any intentional behavioral difference
between this spec (and the resulting reimplementation) and
`main`'s `parse.c` must be recorded here with rationale.

The four divergence-log items currently gated on Phase 4
(per `docs/swap-out-log.md`):

| Commit | Domain | Phase 4 disposition |
|---|---|---|
| `150f17d` | parse: `try_eval_node` FP fix | Port to swap-out during impl; spec already mandates correct float fold (§6). |
| `e7e7393` | codegen: variadic va_start | Phase 5; Phase 4 must surface the trigger condition (variadic prototype with no named args). |
| `ff529fb` | parse: forward-static-fn | Q13.A — port now (§12). |
| `93c6ecc` | parse + codegen: NetBSD bundle | Phase 4/5 jointly; Phase 4 ports the parse-side piece. |

If implementation reveals additional divergences, they land here
with a one-paragraph rationale before the Phase 4 closure tag.

---

## 14. Closure gate (Q9)

Phase 4 closes when **all** of:

1. `scripts/bootstrap_validate.sh` exits 0 (two-stage self-host
   md5 fixed point).
2. Full test corpus passes:
   - `tests/regression/` — all entries including new Phase 4
     contract tests (Q14: A, B, C, D, E, F).
   - `tests/compliance/` — all entries including the new
     forward-static-fn compliance test (§12).
   - `tests/torture/` — gcc torture suite at parity with `main`.
3. `parse_v2.c` is the canonical `parse.c` (rename + commit; old
   `parse.c` deleted).
4. The `phase-4-closed` annotated tag is created at the swap-in
   commit and pushed.
5. `docs/swap-out-log.md` has a Phase 4 closure entry.

The single-tag policy (Q16.A) reflects Q3.A's single big-bang
swap-in choice.  No intermediate `phase-4a-closed` etc. tags.

---

## 15. Phase 5 prerequisites (Q15)

Phase 4 and Phase 5 share the AST contract.  Phase 5 (codegen
swap-out) will rely on the following invariants the parser
provides; recording them here saves Phase 5's inventory effort.

1. **`Obj` list is the parser's output.**  Codegen iterates this
   list; each entry is either a function (with `body` AST) or a
   global (with `init_data` + relocation list).

2. **`Node->ty` is populated for every Node by parse return
   time.**  Codegen does not invoke the type system; it reads
   `Node->ty` directly.  (`add_type` is run by the parser as a
   final pass.)

3. **`Node` line/column metadata is populated for every Node.**
   Used by codegen for `.loc` directive emission (debug info).

4. **Parse-time lowerings have already happened.**  Codegen sees
   no `_Complex` arithmetic operators, no vector arithmetic
   operators, no compound-literal-as-rvalue (compound literals
   are lowered to local declarations + initialization).

5. **VLA size variables are emitted as ordinary locals.** Codegen
   needs no special handling — `__vla_size_N` looks like any
   other `size_t` local.

6. **Static initializer evaluation is parse-time.**  Codegen
   receives `init_data` + relocation records, never raw
   initializer expressions.

7. **Trampoline-based nested functions** (§9): codegen receives
   `outer_vars` lists and is responsible for trampoline code +
   captured-variable lowering.  Phase 5 must inherit this
   responsibility intact.

8. **Labels-as-values:** `&&label` produces a `void *` Node;
   `goto *expr` is a Node kind codegen must materialize.  Phase 5
   scope.

9. **`__atomic_*` and `__sync_*` builtins** are parsed as
   Node kinds; codegen lowers to either inline atomic instructions
   or libcall.  Phase 5 scope; Phase 4 produces the Node shape.

10. **`__builtin_*` family**: most are lowered by the parser to
    direct Node forms (`__builtin_offsetof` to constant,
    `__builtin_va_arg` to a Node kind, etc.).  Phase 5 receives
    those Node kinds, never the raw builtin call.

The reverse direction — what the parser relies on from Phase 1/2
(tokenization, preprocessing) and Phase 3 (type construction) —
is documented in those phases' specs and is stable.

---

## 16. Out of scope

The following are explicitly **not** Phase 4's concern:

- **Tokenization** (Phase 1 / `01_tokenizer.md`).  The parser
  consumes tokens; it does not produce them.
- **Preprocessing** (Phase 2 / `02_preprocessor.md`).  All `#`
  directives have already been resolved.
- **Type construction primitives** (Phase 3 / `03_type.md`).  The
  parser uses `pointer_to`, `array_of`, `func_type`, etc.; it
  does not redefine them.
- **Codegen lowering** (Phase 5).  The parser produces Nodes; it
  does not emit assembly.
- **Optimization passes.** ncc has none.
- **Diagnostic formatting.** Errors are routed through `error.c`;
  the parser supplies positions and messages, not formatting.
- **Trampoline runtime** (§9.3).
- **Real SIMD codegen** (§8.4).

---

## 17. Notes for the spec author

The four sub-files cite this document for cross-cutting
decisions.  When drafting them:

- Cite this document by section number (e.g., "per §5.2",
  "is_typename keyword set per §5.2").
- Do not duplicate cross-cutting content.  If a sub-file finds
  itself re-explaining `is_typename`, reformulate as a citation.
- New cross-cutting concerns discovered during sub-file drafting
  belong here, not in the sub-file.  Update §-numbering and
  re-cite from sub-files.
- The §13 divergence log is shared; sub-files append to it as
  needed.
- The §15 Phase 5 prerequisites list is shared; sub-files append
  AST invariants they discover.

The sub-files take precedence over this document for the
zone-specific behavioral detail they cover; this document takes
precedence for the cross-cutting decisions enumerated above.
