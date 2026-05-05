# ncc Parser — Initializers Sub-Chunk (Phase 4, 04d)

This sub-chunk specifies the **initializer zone** of `src/parse.c`:
designated initializers, brace elision, partial overrides, init-
code generation (the conversion of an `Initializer` tree into
assignment statements), and global initializer evaluation
(compile-time fold + pointer-relative relocations).

Cross-cutting decisions (designated init reference policy, etc.)
live in `04_parse.md`; this file cites them.

**Coverage in `src/parse.c` (per `04_parse_inventory.md`):**

| Lines | Section |
|---|---|
| 4664–5176 | Initializer parsing (designated, brace-elision, partial overrides, string-literal init, complex/vector init) |
| 5177–5352 | Init-code generation |
| 5353–5547 | Global initializer evaluation (compile-time + relocations) |

The local-declaration-with-initializer entry point is in
`04a_decl.md` §H; this sub-chunk picks up at the `=` sign.

**Status:** skeleton.

---

## A. The `Initializer` tree

[Field-by-field reference: ty, expr (for scalar leaves), children
(for aggregates), is_flexible (for the C99 `int a[] = {1,2,3}`
size-from-init form), mem (bookkeeping).  Parallel to the Type
tree the initializer covers.]

### A.1 Construction

[`new_initializer(Type *ty)` builds an empty initializer tree
shaped after `ty`.  Recurses into struct members, array elements,
union (single child for the active member).  Flexible array
members handled per A.3.]

### A.2 Type-driven traversal

[Initializer parsing walks the type tree and the token stream in
lockstep.  At each leaf type, expects a scalar expression; at each
aggregate type, expects either `{...}` or a designator chain
that descends into the aggregate.]

### A.3 Flexible array members

[`int a[] = {1,2,3}` — the array length is computed from the
initializer count.  Stored on the Initializer until parse
completes, then back-propagated to the `Type`.]

## B. Initializer parsing

### B.1 Scalar initialization

[`int x = expr;` — single expression of compatible type.  Implicit
cast inserted by `add_type`.]

### B.2 Aggregate initialization with full braces

[`int a[3] = {1, 2, 3}; struct S s = {1, 2, "foo"};`.  Brace
nesting matches type nesting.]

### B.3 Brace elision (Q11)

[Per C11 §6.7.9/13 + `04_parse.md` §10.  When a sub-aggregate's
`{` is omitted, the parser greedily takes elements until the
sub-aggregate is full, then resumes the outer.  Examples:

```c
int m[2][3] = {1, 2, 3, 4, 5, 6};   // elision: m[0]={1,2,3}, m[1]={4,5,6}
int m[2][3] = {{1, 2, 3}, {4, 5, 6}};  // explicit
int m[2][3] = {1, 2, 3, {4, 5, 6}};   // mixed elision
```

The "full" determination is type-driven: a sub-aggregate of N
elements consumes N elements from the outer brace's element
queue.]

### B.4 Designated initializers

[`.member = expr` for structs/unions, `[index] = expr` for
arrays.  Designators may chain: `.outer.inner = expr`,
`[3].member = expr`.  A designator resets the "current position"
within the aggregate; subsequent un-designated elements continue
from the new position.  Examples:

```c
struct S s = {.b = 2, .a = 1};      // any order
int a[5] = {[3] = 30, [1] = 10};   // sparse
int a[5] = {[1] = 10, 20, 30};     // a[1]=10, a[2]=20, a[3]=30
```]

### B.5 Partial overrides

[A later element targeting the same slot as an earlier element
overwrites:

```c
struct S s = {.a = 1, .b = 2, .a = 3};   // s.a == 3, s.b == 2
```

The parser overwrites the Initializer subtree for the targeted
slot, discarding the earlier value (no side effects of the
discarded initializer expression are emitted).  This is C
standard behavior per C11 §6.7.9/19.]

### B.6 String-literal initialization

[`char s[] = "hello"` produces `s` of size 6 (5 chars + NUL).
`char s[6] = "hello"` is fine; `char s[5] = "hello"` is
permitted (NUL omitted) per C11 §6.7.9/14; `char s[4] = "hello"`
is an error.

Wide strings: `wchar_t s[] = L"hello"`.  Char16/char32 likewise
when the type matches.]

### B.7 Compound literals as initializer values

[`(type){...}` may appear as an initializer expression.  The
inner initializer is parsed recursively; the outer slot receives
the resulting tmp-or-rvalue.]

### B.8 Complex / vector initialization

[`_Complex double z = (1.0 + 2.0i)` — single expression.
`_Complex double z = {1.0, 2.0}` — two-element brace form
(real, imag).  `vec_t v = {1, 2, 3, 4}` — element-by-element.
Per `04_parse.md` §8.]

## C. Designated-init worked examples (Q11.A)

### C.1 Partial override + brace elision

```c
struct S { int a, b, c; };
struct S arr[3] = {
  {1, 2, 3},
  {.b = 5},                  // arr[1].a == 0, arr[1].b == 5, arr[1].c == 0
  [0].c = 99,                // override arr[0].c
};
// arr[0] = {1, 2, 99}
// arr[1] = {0, 5, 0}
// arr[2] = {0, 0, 0}
```

[Walkthrough of how the parser handles each step.]

### C.2 Brace-elided union

```c
union U { int i; char c; };
union U u = {1};   // u.i == 1 (first member)
union U v = {.c = 'a'};   // v.c == 'a'
```

### C.3 Sparse array with extension

```c
int a[5] = {[1] = 10, 20, 30};   // a = {0, 10, 20, 30, 0}
```

[Designator + un-designated continuation.]

## D. Init-code generation (`create_lvar_init`)

[After parsing builds the `Initializer` tree, this pass converts
it to a sequence of `ND_ASSIGN` statements that codegen emits.
For a local declaration, the assignments run at scope entry; for
a global, see §G.]

### D.1 Scalar case

[`Initializer.expr` becomes the RHS of `ND_ASSIGN(var, expr)`.]

### D.2 Aggregate case

[Recurse into children, generating per-leaf assignments.  Use
member-access / array-index Nodes to address each leaf.]

### D.3 String-literal case

[Char-array string init lowers to either:
- A bytewise loop (for runtime-known lengths) — out of scope,
  since strings are static.
- A series of `ND_ASSIGN` per byte (small).
- Memcpy-like bulk initialization (codegen choice).

The parser emits the simplest form (per-byte ND_ASSIGN); codegen
may optimize.]

### D.4 Zero-init for un-mentioned slots

[Per C11 §6.7.9/19, an aggregate's un-mentioned slots are
zero-initialized.  The parser emits `ND_ASSIGN(slot, 0)` for each
or — for performance — relies on a bulk memset emitted by codegen.

Match main's choice.  (Likely: emit ND_MEMZERO Node which codegen
lowers to memset.)]

## E. Compound literal as rvalue

[`(type){init}` in expression position (per `04b_expr.md` §H.5)
also lands here for the init-tree construction; the difference
from a declaration is that the resulting tmp local is anonymous
(uses `new_anon_gvar`-counter for naming) and its scope is the
enclosing block.]

## F. Compound literal as initializer

[Recursive case: an outer initializer's slot is a compound
literal.  The inner initializer parses via the same machinery; the
outer slot receives the inner tmp.]

## G. Global initializer evaluation

### G.1 Compile-time scalar fold

[For a `static int x = 1 + 2 * 3;` global, the parser folds the
expression via `eval_node` and stores the result as `init_data`
(raw bytes).  The Obj has no body / runtime init.]

### G.2 Pointer-relative relocations

[`static int *p = &arr[3];` — the value is `arr_address + 12`.
The parser cannot fold this to bytes (the linker resolves
`arr_address`); instead it emits a relocation record on the Obj
identifying the symbol and the offset.  Codegen + assembler emit
the appropriate `.long arr+12` directive.]

### G.3 Forward static fn references (Q13)

[Per `04_parse.md` §12 / `ff529fb` fix: a reference to a
forward-declared static function is permitted in a file-scope
initializer.  The parser records the reference; if the function
is defined later in the same translation unit, the reference
resolves; if not, error at end-of-translation-unit.]

### G.4 String-literal globals

[`static const char *s = "hello"` — the string literal becomes an
anonymous global (via `new_anon_gvar`); `s` gets a relocation
record pointing at it.]

### G.5 Bit-field globals

[`struct S { int x : 5; } s = {12};` — the parser computes the
storage-unit byte pattern at compile time and emits as raw
`init_data`.  Endianness of the host is not relevant — ARM64 is
little-endian, which is the only target.]

## H. Worked examples

### H.1 `int (*p)(int) = &g;` where `g` is a static fn

[Trace through ordinary-namespace lookup, address-of, relocation
record emission.]

### H.2 `static const char *msgs[] = {"hi", "bye", NULL};`

[Anonymous globals for each string, relocation records, NULL as
constant.]

### H.3 `struct S s = {.a = (1 + 2), .b = 3};`

[Designator → fold via `eval_node` → init_data emission.]

### H.4 `_Complex double z = {1.0, 2.0};`

[Two-element brace form; Initializer tree shape; init bytes.]

## I. Phase 5 prerequisites added by 04d

[AST invariants from this sub-chunk; append to `04_parse.md` §15
during impl review.  Specifically the `init_data` + relocation
record format.]

## J. Open questions

[Sub-chunk-specific questions raised during drafting.]
