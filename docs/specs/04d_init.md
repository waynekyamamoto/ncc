# ncc Parser — Initializers Sub-Chunk (Phase 4, 04d)

This sub-chunk specifies the **initializer zone** of `src/parse.c`:
the `Initializer` and `InitDesig` data shapes, designated
initializer parsing including brace elision and partial overrides
per `04_parse.md` §10, init-code generation for local variables,
compile-time evaluation for global variables, and the relocation
record format codegen consumes.

Cross-cutting decisions live in `04_parse.md`; this file cites
them.

**Coverage in `src/parse.c`:**

| Lines | Section |
|---|---|
| 4664–4687 | `Initializer` and `InitDesig` data structures |
| 4689–4734 | `new_initializer` (tree construction) |
| 4743–4860 | `initializer2` (top-level dispatcher) |
| 4862–4892 | `string_initializer` |
| 4894–4983 | `array_initializer1` |
| 4985–4987 | `is_end` helper |
| 4989–5083 | `struct_initializer1` |
| 5085–5143 | `union_initializer` |
| 5145–5175 | `count_array_init_elements` (size-from-init for flexible arrays) |
| 5178–5275 | `create_lvar_init` (init-code generation) |
| 5277–5303 | `init_desig_expr` (lvalue designator chain) |
| 5305–5350 | `lvar_initializer` (entry from `04a_decl.md` §H) |
| 5358–5404 | `gvar_initializer` |
| 5406–end of section | `write_gvar_data` (Initializer → bytes) |

**Status:** substantive.

When this document and `main`'s observable behavior disagree, this
document is wrong and must be updated, unless the divergence is
recorded in §13 of `04_parse.md`.

---

## A. The `Initializer` data structure

`Initializer` is a tree mirroring the structure of the type being
initialized.  It is constructed by `new_initializer(ty,
is_flexible)` and consumed by either `create_lvar_init` (for
locals) or `write_gvar_data` (for globals).

```c
struct Initializer {
  Initializer *next;         // sibling in some linked-list contexts
  Type *ty;                  // the type this initializer covers
  Token *tok;                // representative token (for diagnostics)
  bool is_flexible;          // size discovered from init (flexible array)
  Initializer **children;    // for aggregates (array/vector/struct/union)
  Node *expr;                // for scalar leaves
};
```

A leaf has `expr` set; an aggregate has `children` allocated to
the right length, with each child recursively initialized.

```c
struct InitDesig {
  InitDesig *next;
  int idx;          // array index
  Member *member;   // struct member
  Obj *var;         // root variable (set on the outermost desig)
};
```

`InitDesig` is the address-chain walker: when emitting per-leaf
assignments, the chain encodes "var.member.array[idx]..." from
root to leaf.

### A.1 `new_initializer(ty, is_flexible)` construction

For each type kind:

| `ty->kind` | Action |
|---|---|
| `TY_ARRAY` with `array_len < 0` | Set `is_flexible = true`; no `children`. |
| `TY_ARRAY` with `array_len == 0` and `is_flexible` arg | Same: flexible. |
| `TY_ARRAY` with positive `array_len` | Allocate `children[array_len]`, recurse on `ty->base` for each. |
| `TY_VECTOR` | Allocate `children[array_len]`, recurse on `ty->base`. |
| `TY_STRUCT` / `TY_UNION` | Allocate `children[member_count]`, recurse on each member's type. |
| Other (scalars, pointers) | Leaf — no `children`, `expr` filled later. |

### A.2 `is_flexible` propagation

For struct types containing a flexible array member (`ty->is_
flexible == true`), the **last** member's child initializer is
recursively constructed with `is_flexible = true` (regardless of
the outer arg; specifically: `is_flexible_arg && ty->is_flexible
&& !mem->next`).

This makes `int n; char data[];` accept `= { 5, "hello" }` and
have `data` resize to length 6.

### A.3 Flexible array detection

After `initializer2` finishes, the flexible-flag site has been
filled with a concrete-length child.  `lvar_initializer` and
`gvar_initializer` then update `var->ty` so the variable's
declared type reflects the initialized size:

```c
if (var->ty->kind == TY_ARRAY && var->ty->array_len < 0)
  var->ty = init->ty;
```

---

## B. `initializer2` (the top-level dispatcher)

Called once per init-position with the `Initializer` slot to fill.
Token-driven dispatch:

### B.1 String literal initializing a `char []`

If `init->ty` is array-of-char and the next token is a string
literal: dispatch `string_initializer`.

### B.2 Array

If `init->ty->kind == TY_ARRAY`: dispatch `array_initializer1`
(with or without leading `{`; the helper detects).

### B.3 Vector

If `init->ty->kind == TY_VECTOR`:
- If next token is `{`: `array_initializer1` (treat like array
  init).
- Otherwise: parse a single `assign` expression.
  - If its type is `TY_VECTOR`: store in `init->expr` (whole-
    vector copy).
  - Otherwise: **broadcast** the scalar to every element by
    setting each `init->children[i]->expr` to the same node.
    (This is GCC's broadcast semantics for scalar-to-vector
    init.)

### B.4 Struct

If `init->ty->kind == TY_STRUCT`:
- If next is `{` or `.`: `struct_initializer1`.
- If next is a string literal **and** the struct has members:
  walk the first-member chain looking for a char-array.  If
  found, dispatch `string_initializer` on that target.  This
  handles `struct { char w[8]; } q[2] = {"abc", "def"}` per
  C11 §6.7.9/14 brace-elision rules.
- Otherwise parse `assign`:
  - If its type is `TY_STRUCT`: store in `init->expr` (whole-
    struct copy).
  - Otherwise: descend into the first member of nested structs
    and arrays until a scalar is found, set its `expr`.  This
    is the partial-init case where `struct S s = expr;` covers
    only the first member.

### B.5 Union

If `init->ty->kind == TY_UNION`:
- If next is **not** `{` and not `.`: try `assign`.  If its
  type is `TY_UNION`, store as whole-union copy; otherwise
  rewind and fall through.
- Dispatch `union_initializer`.

### B.6 Scalar

Else (scalar leaf):
- If next is `{`: recurse `initializer2` on the inner content,
  consume optional `,`, skip `}`.  This handles `int x = {5};`
  and the GCC-brace-around-scalar form.
- Otherwise: `init->expr = assign(rest, tok)`.

---

## C. `array_initializer1`

Handles array (and vector, when used array-style) initializers
with full support for:
- Optional leading `{`.
- Designated initializers `[i] = val`.
- Range designators `[lo ... hi] = val` (GCC extension).
- Multi-level designators `[outer][inner] = val`.
- Excess elements (silently parsed and discarded).
- Trailing comma.
- Brace-less mode: stop at outer-scope tokens.

### C.1 Setup

1. `has_brace = consume(&tok, tok, "{")`.
2. If `init->is_flexible`: call `count_array_init_elements` to
   compute the actual length, then re-construct via `*init =
   *new_initializer(array_of(base, len), false)`.

### C.2 Main loop

For `i = 0; !is_end(tok); i++`:

**Comma handling (after first iteration):**
- In brace-less mode, if next is `,` followed by `.`: the
  parent struct's next field is starting; break (don't consume
  the comma).
- In brace-less mode, if next is `,`, `i >= array_len`, and
  next-next is not `[`: parent-array continuation; break.
- Otherwise consume `,`.
- Trailing comma: if `is_end(tok)` after comma, break.

**Element processing:**
- If `i >= array_len` and next is not `[`: stop (no more slots,
  no designators upcoming).
- If next is `[`: designated.
  - Parse `lo = const_expr_val`.
  - If next is `...`: range designator, parse `hi`.  Skip `]`,
    optional `=`.  Parse the value once via `initializer2` (so
    struct/array values are handled), then for each `j` in
    `lo..hi`, replay the same token range against `init->
    children[j]`.  Set `i = hi`, continue.
  - Else: `i = lo`, skip `]`.  Multi-level: if brace-less and
    next is `[`, pass through to `initializer2` (the inner `[`
    becomes the child's designator).  Otherwise consume
    optional `=`, dispatch `initializer2` to the child.  In
    brace-less mode, break after a designated element (parent
    handles continuation).
- Otherwise: `initializer2(init->children[i])`.

### C.3 Excess element handling

After the main loop, if `has_brace`:
- Loop until `}` consumed.  If next token is unrecognized, skip
  via `assign(...)` or single-token advance.  This silently
  discards excess initializers — matches GCC's permissive
  behavior.

The excess-skip loop tolerates `,`, unknown tokens, and `assign`-
parseable expressions.  No error; this matches `main`'s lenient
acceptance of over-initialization.

### C.4 Partial overrides

A range designator that overlaps an earlier-initialized region
overwrites: each iteration of the `lo..hi` loop replays
`initializer2` against `init->children[j]`, which writes into
its `expr` field.  The previous expression is discarded
(its side effects are not emitted; per C11 §6.7.9/19, the
later designator wins).

---

## D. `struct_initializer1`

Handles struct initializers with designated `.field = val` and
old-GCC `field: val` syntax, brace elision, partial overrides.

### D.1 Setup

`has_brace = consume(&tok, tok, "{")`.  Track `mem` (the
"current" member, advanced after each non-designated init) and
`i` (iteration counter).

### D.2 Main loop

For each iteration:

**Comma after first**: same trailing-comma handling as
`array_initializer1`.

**Designated form: `.field = val`** or **old-style `field:
val`**:
- `old_style` is true if the form is `IDENT :`.
- Look up the member via `get_struct_member` (which recurses
  into anonymous struct/union members).
- If the member is not found: rewind to before-comma and break.
  This enables outer-scope handling — when an inner struct has
  no such member, control returns to the outer initializer
  which may have it.
- Walk the member chain, descending into anonymous member
  children to reach the target.
- **Nested designator**: if next is `.` or `[`, recurse
  `initializer2(target)` — the chain `.a.b` or `.a[0]` is
  handled within `initializer2`.
- Otherwise consume `:` (old style) or optional `=` (new
  style), then `initializer2(target)`.
- Advance `mem = mem->next` so subsequent un-designated entries
  continue from after the designator.

**Non-designated**:
- If `mem` is NULL (past the end): break.
- Find the member's index by walking `init->ty->members`.
- `initializer2(init->children[idx])`.
- Advance `mem`.

### D.3 Excess-element skip

Same as `array_initializer1`: if `has_brace`, consume excess
tokens until `}`.

### D.4 Anonymous struct member access

`get_struct_member` (`04b_expr.md` §N.1) walks anonymous-member
chains.  When a designator names a deeply-nested member
(`.outer.inner`), `struct_initializer1` walks the chain a
second time at the `Initializer` level:

```c
Initializer *target = init;
for (;;) {
  // scan target->ty->members
  // if direct match: target = target->children[idx]; done
  // if anonymous and contains the target: target = target->
  //   children[idx]; continue search at the new level
}
```

The accumulated descent gives the right child slot to fill.

---

## E. `union_initializer`

Unions initialize one member at a time.  Designators select
which member; un-designated init goes to the first member.

### E.1 Empty `{}`

If `has_brace` and immediately `}`: zero-initialize the union
(no `expr` set on any child).  Return.

### E.2 Loop

Without braces: process exactly one element, then return.
With braces: process all elements (later ones overwrite via
partial-override semantics).

For each element:
- If next is `.IDENT` or `IDENT :`: find the member, parse
  designator (with possible nested `.` or `[`), dispatch
  `initializer2` to the child.
- Otherwise: dispatch `initializer2` to `init->children[0]`
  (the first member), then break.

### E.3 Closing brace

If `has_brace`: consume optional trailing comma, skip `}`.

---

## F. `string_initializer`

Char-array string initialization.

### F.1 Adjacent-literal concatenation

```c
char s[] = "abc" "def";
```

Walk `tok->next` as long as the next token is also `TK_STR`,
concatenating into a single buffer of total length.  Each
concatenated literal contributes `array_len - 1` bytes (the
preceding NUL is dropped); a final NUL is added.

### F.2 Flexible-array resolution

If `init->is_flexible`, replace the initializer with a fresh one
whose array length matches the string length:
```c
*init = *new_initializer(array_of(init->ty->base, str_len), false);
```

### F.3 Per-byte child population

For `i = 0..min(array_len, str_len) - 1`:
```c
init->children[i]->expr = new_num(str[i], start);
```

Char arrays of fixed length larger than the string get the rest
zero-filled (by virtue of the `ND_MEMZERO` prefix in
`lvar_initializer`).

Char arrays of fixed length smaller than the string: the excess
bytes are not written.  Per C11 §6.7.9/14, this is permitted as
long as the string is at most one byte longer than the array
(the trailing NUL is allowed to be absent).  ncc's
implementation does not strictly check the C11 size limit;
matches `main` lenient behavior.

---

## G. `lvar_initializer` (init-code generation entry)

```c
static Node *lvar_initializer(Token **rest, Token *tok, Obj *var) {
  Initializer *init = new_initializer(var->ty, true);
  initializer2(rest, tok, init);
  // Update flexible array type from resolved init
  if (var->ty->kind == TY_ARRAY && var->ty->array_len < 0)
    var->ty = init->ty;
  // memzero followed by per-leaf assignments
  ...
}
```

### G.1 Memzero prefix

Every local declaration with an initializer first emits an
`ND_MEMZERO` node referencing `var`.  Codegen lowers this to a
`bzero`-equivalent over `var->ty->size` bytes.

This guarantees C semantics: un-initialized aggregate slots are
zero-valued.  The per-leaf assignments that follow are
overlayed on top of the memzero.

The cost of always-memzero is small (most locals are initialized
densely); the alternative — tracking which slots are written
and only zeroing the gaps — is more complex and error-prone.

### G.2 Single-expression aggregate init

If `init->expr` is set (whole-struct, whole-union, whole-vector,
or whole-complex copy):
```c
return ND_COMMA(memzero, ND_ASSIGN(var, init_val))
```
With complex-cast adjustment (per `04b_expr.md` §C.1) if the
RHS is a different complex shape.

### G.3 Per-leaf assignment chain

Otherwise, `create_lvar_init(init, var->ty, &desig, tok)`
returns a linked list of `ND_ASSIGN` nodes (chained via
`->next`).  Fold them into a left-leaning chain of `ND_COMMA`
nodes, with `memzero` as the leftmost:

```
ND_COMMA(ND_COMMA(...ND_COMMA(memzero, assign1), assign2)..., assignN)
```

This evaluation order ensures memzero runs first, then per-leaf
assignments left-to-right.

### G.4 No initializer

If `init->expr` and `rhs` are both empty (initializer parsed to
no leaves), return just the `memzero`.

---

## H. `create_lvar_init` (per-leaf chain builder)

Recursive walker.  For each Type kind:

### H.1 Array

For `i = 0..array_len - 1`:
- Build `InitDesig next = { desig, idx = i }`.
- Recurse on `init->children[i]` with `ty->base`.
- Append the returned chain to the result.

### H.2 Vector

If `init->expr`: whole-vector copy.  Build `lhs = init_desig_
expr(desig)`, return `ND_ASSIGN(lhs, init->expr)`.

Otherwise per-element: for each `i` with non-NULL
`children[i]->expr`, build a byte-arithmetic addressing
expression (avoiding vector-decomposition):
```
*((elem_t *)((char *)&vec + i * elem_size))
```
This bypasses the vector machinery so codegen sees a normal
scalar store.  The byte-arithmetic ensures `&vec` produces a
valid base address even when the vector is part of a larger
structure.

### H.3 Struct

If `init->expr`: whole-struct copy via `ND_ASSIGN`.

Otherwise per-member: for each member, build a child `desig`
with `member = mem`, recurse.

### H.4 Union

Find the first child with a populated `expr` or `children`
(i.e., the active member).  Build a child `desig` with
`member = mem`, recurse.  If no member is populated, fall back
to the first member.

### H.5 Scalar leaf

If `!init->expr`, return NULL (no assignment needed; memzero
already covered the slot).

Otherwise:
1. `lhs = init_desig_expr(desig, tok)`.
2. `rhs = init->expr`.
3. Apply complex-cast adjustments per `04b_expr.md` §C.1.
4. Return `ND_ASSIGN(lhs, rhs)`.

### H.6 Bit-field initialization

The bit-field handling for **local** initializers passes
through the standard `ND_ASSIGN` path with a `ND_MEMBER` lhs
that has `is_bitfield = true`.  Codegen recognizes this and
emits the read-modify-write pattern.

---

## I. `init_desig_expr` (lvalue chain)

Walks an `InitDesig` chain (innermost-to-outermost via the
`next` pointer) to produce an lvalue Node.

### I.1 Root variable (`desig->var`)

Returns `new_var_node(desig->var, tok)`.  This is the
termination case — only the outermost designator has `var` set.

### I.2 Member access (`desig->member`)

Recursively build the parent lvalue, then wrap in `ND_MEMBER`:
```c
Node *node = new_unary(ND_MEMBER, init_desig_expr(desig->next, tok), tok);
node->member = desig->member;
return node;
```

### I.3 Array/vector index (`desig->idx`)

Build the parent lvalue.  `add_type` to determine whether the
parent is a vector.

For vector: byte-arithmetic addressing (per H.2):
```
*(elem_t *)((char *)&parent + idx * elem_size)
```

For array: `*(parent + idx)` via `new_add` then `ND_DEREF`.

The two paths produce different code shapes (array uses
`new_add` which respects pointer-arithmetic scaling; vector
uses raw byte arithmetic to avoid vector-decomposition).

---

## J. `gvar_initializer` (global initializer entry)

Called from `04a_decl.md` §I.6 / §H.3 (file-scope globals and
static locals).

### J.1 Construction

1. `init = new_initializer(var->ty, true)`.
2. `initializer2(rest, tok, init)`.
3. If `var->ty` is incomplete array, update from `init->ty`.

### J.2 Allocation size

For non-flexible cases, `alloc_size = var->ty->size`.

For struct-with-flexible-member: walk to the last member, take
its initializer's resolved type, compute:
```
alloc_size = last_offset + flex_init->ty->size
```

For incomplete element type (`alloc_size < 0`): clamp to 0.

### J.3 Buffer allocation

`buf = calloc_checked(1, alloc_size)` (zero-initialized — same
guarantee as the local memzero, but baked into static storage
instead of a runtime memset).

### J.4 Walk the initializer

`write_gvar_data(init, var->ty, buf, 0, &rel_tail)` walks the
tree, writing each leaf into `buf` at its computed offset.

### J.5 Finalize

```c
var->init_data = buf;
var->init_data_size = alloc_size;
var->rel = head.next;
```

The `Relocation` chain `head.next` carries pointer-relative
relocations discovered during the walk.

---

## K. `write_gvar_data` (Initializer → bytes + relocations)

Recursive walker mirroring `create_lvar_init` but writing to a
byte buffer instead of building Node assignments.

### K.1 Array, Vector

For each `i` in `0..array_len`:
- `write_gvar_data(children[i], base, buf, offset + base->size *
  i, rel_tail)`.

### K.2 Struct

For each member:

**Bit-field**: evaluate `init->children[i]->expr` via
`eval2(NULL)`, mask to `bit_width`, OR into the storage unit at
`offset + mem->offset` shifted by `mem->bit_offset`.  The
storage unit size is `mem->ty->size`; the read-modify-write is
done via `memcpy` to avoid strict-aliasing concerns.

**Non-bit-field flexible array member** (last member in
flexible struct): use the initializer's resolved type
(`init->children[i]->ty`) instead of the declared member type
to size the array correctly.

**Otherwise**: recurse with `offset + mem->offset`.

### K.3 Union

Find the first child with `expr` or `children` populated
(the active member); recurse into it at the same `offset`.

### K.4 Scalar leaf

If no `expr`: return (zero-fill from calloc).

Evaluate the expression:
- **Complex**: extract real and imaginary halves via `ND_REAL`
  / `ND_IMAG`, evaluate each via `eval_double` /
  `eval_complex`, write the bytes for each half at the
  appropriate sub-offset.
- **Float**: `eval_double`, store as IEEE 754 bytes
  (`memcpy(buf + offset, &dval, ty->size)`).
- **Pointer or integer with a label** (relocation case):
  - `eval2(expr, &label)` returns the addend (offset from the
    symbol).
  - Append a `Relocation` to the chain: `{ offset =
    buf_offset, addend, label }`.
  - The buffer bytes are unset (zero from calloc); codegen
    emits the symbol-reference directive at link time.
- **Pointer or integer no label**: `eval2(expr, NULL)` returns
  the value; write its bytes.

### K.5 String literal as initializer

`char s[] = "hello"` produces an aggregate where each
`children[i]->expr` is a `ND_NUM` for one character.  Walking
those via the array case writes 6 bytes (5 chars + NUL).

`char *s = "hello"` is different: the RHS is a pointer to an
anonymous string literal global.  `eval2` produces the
relocation, and a single 8-byte slot (pointer) is filled with
`{label of anon-global, addend = 0}`.

### K.6 Compound literal as initializer value

`(struct T){...}` in initializer position produces an anonymous
file-scope global whose own `init_data` was populated by a
recursive `gvar_initializer` call.  At the outer site, `eval2`
sees the `ND_VAR` for the anonymous global and produces a
relocation referencing it.

### K.7 Forward-declared static fn references (Q13 / `ff529fb`)

Per `04_parse.md` §12: a reference to a forward-declared static
function can appear in a file-scope initializer.  The
relocation records the function name; resolution at end-of-
translation-unit verifies that a definition exists.

---

## L. The `Relocation` record

`Relocation` (declared in `cc.h`) records a pointer-relative
constant in static initializer data.  Codegen emits it as an
assembler `.long` / `.quad` directive referencing the named
symbol with an addend.

```c
typedef struct Relocation {
  struct Relocation *next;
  int offset;       // byte offset within init_data
  char **label;     // pointer-to-pointer-to-name for late binding
  long addend;      // constant offset added to the resolved address
} Relocation;
```

The `char **label` indirection deserves explanation: at
relocation-recording time, the symbol's final name might not be
known (e.g., the symbol is a forward declaration whose mangled
name is set later).  Storing `label = &symbol->name` means that
when codegen reads `*label` at emission time, it gets the
final name even if the symbol was renamed in between.

For the common case where the name is already final, `*label`
is just the static name — but the indirection is harmless.

---

## M. Worked examples (Q11.A worked-examples requirement)

### M.1 Partial override + brace elision

```c
struct S { int a, b, c; };
struct S arr[3] = {
  {1, 2, 3},
  {.b = 5},
  [0].c = 99,
};
```

Trace:

1. `arr` is `array_of(struct_S, 3)`.  `new_initializer` builds
   3-element children, each a struct-shaped Initializer with 3
   leaf children.
2. `array_initializer1` consumes `{`.  Iteration 0:
   - First `{1,2,3}`: dispatches `initializer2` →
     `struct_initializer1` for `arr[0]`.  Three positional
     non-designated members:
     - `arr[0].a.expr = 1`
     - `arr[0].b.expr = 2`
     - `arr[0].c.expr = 3`
3. Iteration 1: `{.b = 5}`:
   - `struct_initializer1` for `arr[1]`.
   - `.b = 5`: designated → set `arr[1].b.expr = 5`.
   - End of brace.  `arr[1].a.expr` and `arr[1].c.expr` remain
     NULL.
4. Iteration 2: `[0].c = 99`:
   - Top-level `[` → designated array entry.  `lo = 0`.
   - Skip `]`.  Brace-less mode: next is `.`, so multi-level
     designator, pass through to `initializer2` for `arr[0]`.
   - `struct_initializer1`: `.c = 99` → overwrites `arr[0].c.
     expr` from `3` to `99`.
5. End of `arr` brace.

Final tree:
```
arr[0] = {1, 2, 99}     // c overwritten by partial override
arr[1] = {0, 5, 0}      // a, c zero-initialized (memzero)
arr[2] = {0, 0, 0}      // entirely uninitialized
```

For local: `lvar_initializer` emits `ND_MEMZERO(arr)` followed
by per-non-NULL-leaf `ND_ASSIGN`.  `arr[0].c` gets one assign
to `99` (the earlier `3` is dropped from the Node tree — its
side effects are not emitted; per C11 §6.7.9/19, the later
designator wins).

For global: `write_gvar_data` walks each leaf and writes its
value (or skips if unset, since calloc gave zero bytes).
`arr[0].c` byte slot is `99`; `arr[1].a` and `arr[1].c` are
the calloc'd zeros.

### M.2 Brace-elided union

```c
union U { int i; char c; };
union U u = {1};       // u.i == 1 (first member)
union U v = {.c = 'a'};
```

For `u`:
- `union_initializer` with `has_brace = true`.
- First iteration: not `.`, not `IDENT :`.  Dispatch
  `initializer2(u.i)` with `1`. Break.
- Skip closing `}`.

For `v`:
- `.c = 'a'`: designated.  Find member `c`, dispatch
  `initializer2(v.c)` with `'a'`.  Break.

Globals: `write_gvar_data` finds the first child with `expr` →
writes that one's value to `offset 0`.  Unions all share the
same offset so the choice of member doesn't change layout.

### M.3 Sparse array

```c
int a[5] = {[1] = 10, 20, 30};
// a = {0, 10, 20, 30, 0}
```

Trace:
1. `array_initializer1`, `has_brace = true`.
2. `i = 0`: not `[`, but the first iteration sees `[`.
   - `[1]` → `i = 1`, skip `]`, optional `=`, dispatch
     `initializer2(a[1])` with `10`.  Result: `a[1].expr = 10`.
3. `i++` → `i = 2`.  Comma, then `20`: positional →
   `a[2].expr = 20`.
4. `i = 3`: comma, then `30` → `a[3].expr = 30`.
5. End brace.

`a[0]` and `a[4]` remain NULL → memzero / calloc zero.

### M.4 String-literal char-array brace elision

```c
struct { char w[8]; } q[2] = { "abc", "def" };
```

Trace:
1. `q` is array of struct-with-char-array.
2. `array_initializer1` for `q`, iteration 0:
   - Token is `"abc"`, a string literal.  But `q[0]` is a
     struct, not a char array.
   - `initializer2(q[0])`: in the struct branch, detect string
     literal + first member is char array → dispatch
     `string_initializer(q[0].w)`.
   - Bytes `'a','b','c','\0'` written to `q[0].w[0..3]`.
3. Iteration 1: `"def"` initializes `q[1].w` similarly.

### M.5 Flexible array member

```c
struct S { int n; char data[]; };
struct S s = { 5, "hello" };
```

Trace:
1. `new_initializer(struct_S, is_flexible=true)`.  Last member
   `data` gets `is_flexible = true` because parent struct
   `is_flexible` and it's the last member.
2. `struct_initializer1`:
   - Iteration 0: positional `5` → `s.n.expr = 5`.
   - Iteration 1: `"hello"` → `string_initializer(s.data)`:
     - `data->is_flexible = true`, so re-create as
       `array_of(ty_char, 6)`.
     - Write bytes `h,e,l,l,o,\0`.
3. `gvar_initializer` (assuming file scope):
   - `alloc_size`: detect `is_flexible` struct, use
     `last->offset + flex_init->ty->size = 4 + 6 = 10`.
   - calloc 10 bytes, walk via `write_gvar_data`.
   - Bytes 0–3: int `5`.
   - Bytes 4–9: `"hello\0"`.
   - `init_data_size = 10` (overrides `ty->size = 4`).
4. `s.ty` is updated post-init to reflect the resolved
   member-type, so `sizeof(s)` returns 10.

### M.6 Pointer-relative relocation

```c
extern int arr[10];
static int *p = &arr[3];
```

Trace:
1. `gvar_initializer` for `p`.
2. `init->expr = ND_ADDR(ND_DEREF(ND_ADD(arr, 3)))` (after
   pointer-arithmetic lowering in `04b_expr.md` §J.1).
3. `write_gvar_data`: scalar leaf, type is pointer.
4. `eval2(expr, &label)`:
   - `ND_ADDR` → `eval_rval(operand, label)`.
   - Operand is `ND_DEREF(ND_ADD(arr, 3))`.
   - `eval_rval(ND_DEREF, label)` → `eval2(ND_ADD, label)`.
   - `ND_ADD(arr, 12)` (pointer-arith already scaled 3 ×
     sizeof(int) = 12).
   - `eval2(ND_ADD)` walks both: `eval2(arr, label)` produces
     `*label = &arr->name`, returns 0.  RHS is 12.  Sum is 12.
5. Relocation: `{ offset = 0, label = &arr->name, addend = 12 }`.
6. `p->init_data` is 8 bytes of zero (the slot is filled by
   the linker).
7. Codegen emits `.quad arr+12`.

---

## N. Phase 5 prerequisites added by 04d

Append to `04_parse.md` §15 master list during impl review:

1. **`Obj.init_data` is the byte buffer.**  Codegen emits its
   bytes via `.byte` directives (or `.long`/`.quad` for sized
   leaves).  The buffer's length is `Obj.init_data_size`,
   which may exceed `Obj.ty->size` for flexible array members.

2. **`Obj.rel` is the relocation list.**  For each entry,
   codegen emits a `.long`/`.quad` directive at `offset` that
   references `*label + addend`.

3. **`Relocation.label` is `char **`** (pointer to pointer to
   name).  At codegen time, dereference once to get the final
   symbol name.

4. **`ND_MEMZERO` Node** with `var` set to the local Obj.
   Codegen emits a memset-zero of `var->ty->size` bytes.

5. **Bit-field global initialization** uses
   `mem->bit_offset`, `mem->bit_width`, `mem->ty->size`
   already populated by `04a_decl.md` §F.5 — no new fields.

6. **Vector and complex global initialization** writes raw
   bytes per element/half — codegen sees ordinary
   `init_data`, no special handling needed.

---

## O. Cross-references

- Local-declaration entry: `04a_decl.md` §H.5 (calls
  `lvar_initializer`).
- Global-declaration entry: `04a_decl.md` §I.6 (calls
  `gvar_initializer`).
- Flexible array member type: `04a_decl.md` §F.4.
- Bit-field layout: `04a_decl.md` §F.5.
- Constant expression evaluation in initializers: `04b_expr.md`
  §K (especially §K.5–K.6 for pointer relocation patterns).
- Anonymous compound literal as initializer value:
  `04b_expr.md` §H.1, §K.6.
- Designated-initializer policy (Q11.A): `04_parse.md` §10.

---

## P. Open questions

### P.1 Excess initializer silently discarded

Per §C.3 / §D.3 / array_initializer1 `has_brace` excess loop:
extra elements past the array/struct end are silently skipped.
C11 §6.7.9/2 says the program is malformed; GCC emits a
warning.

ncc has no warning pass.  This is a known leniency.

Disposition: keep as-is.  Document in §13 of `04_parse.md` if
desired; not blocking Phase 4 closure.

### P.2 Block-level `_Static_assert` inside structs (cross-link)

Struct member-list `_Static_assert` is parsed and evaluated
(per `04a_decl.md` §F.3); the result is verified.  Block-level
inside a function body is silent (per `04c_stmt.md` §I.1).

Disposition: track in `04c_stmt.md` §I.1.

### P.3 Initializer side effects

A range designator `[lo ... hi] = expr` parses `expr` once via
`initializer2` and replays the token range for each `j`.  If
`expr` has side effects (function call, increment), they
execute `hi - lo + 1` times — at least at parse time.  At
codegen time, the per-leaf `ND_ASSIGN` repeats the side
effects in the emitted code.

For globals, where `expr` must be a constant, this is a
non-issue.

For locals, the C standard does not specify; ncc's behavior
matches "expression is re-evaluated per element" (as if you
had written `arr[lo] = f(); arr[lo+1] = f(); ...`).

Disposition: document in §13 of `04_parse.md` for completeness.

### P.4 String literal exact-fit `\0` truncation

C11 §6.7.9/14 permits `char s[5] = "hello"` (exact NUL
truncation).  ncc accepts both this and `char s[3] = "hello"`
(early truncation, with no diagnostic).

Disposition: leniency intentional per §F.3 above.  Would be
straightforward to add the exact-size check; not Phase 4
priority.
