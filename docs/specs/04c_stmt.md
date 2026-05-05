# ncc Parser — Statements & Functions Sub-Chunk (Phase 4, 04c)

This sub-chunk specifies the **statement zone** of `src/parse.c`:
control-flow statements, labels, function definition bodies,
inline asm parsing, the scope mechanics those rely on, and the
nested-function support code (chain-pointer threading, captured-
variable rewriting, non-local goto via longjmp).

Cross-cutting decisions live in `04_parse.md`; this file cites
them.  Local-declaration parsing (the declaration branch inside
`compound_stmt`) is in `04a_decl.md` §H.

**Coverage in `src/parse.c`:**

| Lines | Section |
|---|---|
| 9–113 | Scope structures, push/pop, lookup |
| 53–72 | `CapturedVar` and nested-function declarations |
| 110–113 | `new_unique_name` (label minter) |
| 4034–4436 | `stmt` (all control-flow + asm + labeled stmt) |
| 4439–4510 | `compound_stmt` (block, with __label__ and _Static_assert) |
| 4512–4535 | `parse_typedef` (also reachable from top-level) |
| 4537–4546 | `is_function_definition` (lookahead test) |
| 4651–4661 | `expr_stmt` |
| 5662–5865 | `function` (function definition entry from §I of 04a_decl.md) |
| 5867–5929 | `rewrite_nested_var_refs` |
| 5931–6089 | `nested_function` |

**Status:** substantive.

When this document and `main`'s observable behavior disagree, this
document is wrong and must be updated, unless the divergence is
recorded in §13 of `04_parse.md`.

---

## A. Scope mechanics

### A.1 `Scope` data structure

```c
typedef struct VarScope {
  VarScope *next;
  char *name;
  Obj *var;          // ordinary variable / function
  Type *type_def;    // typedef binding (mutually exclusive with var/enum_*)
  Type *enum_ty;     // enum constant: which enum
  int enum_val;      // enum constant: value
} VarScope;

typedef struct TagScope {
  TagScope *next;
  char *name;
  Type *ty;
} TagScope;

struct Scope {
  Scope *next;
  VarScope *vars;    // ordinary namespace
  TagScope *tags;    // tag namespace
};
```

A `VarScope` entry binds **one** of: a variable (`var`), a
typedef (`type_def`), or an enum constant (`enum_ty` +
`enum_val`).  Mutually exclusive — they share the same slot in
the ordinary namespace per C11 §6.2.3.

The label namespace (`04_parse.md` §4.1) is **not** a `Scope`
field.  Labels are tracked per-function via the file-scope
`labels` and `gotos` linked lists, reset at function entry and
resolved at function exit.

### A.2 `enter_scope` and `leave_scope`

```c
static void enter_scope(void) {
  Scope *sc = calloc_checked(1, sizeof(Scope));
  sc->next = scope;
  scope = sc;
}

static void leave_scope(void) {
  scope = scope->next;
}
```

Push prepends; pop replaces.  No reference counting, no slab
release — entries persist in the allocator until process exit.

The matching `enter_scope` / `leave_scope` invariant per
`04_parse.md` §4.3 is enforced by code inspection only; runtime
mismatches would mean a permanent leak (acceptable; ncc is a
short-lived process).

### A.3 `find_var` and `find_tag`

```c
static VarScope *find_var(Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->next)
    for (VarScope *vs = sc->vars; vs; vs = vs->next)
      if (vs->name && tok->len == strlen(vs->name) &&
          !strncmp(tok->loc, vs->name, tok->len))
        return vs;
  return NULL;
}
```

Linear search inward to outward.  First match wins; later (outer)
bindings shadowed by inner duplicates are unreachable.

`find_tag` analogous on the tag list.

The string compare uses both length and prefix because token
spellings are not NUL-terminated (they're spans into the source
buffer); the binding name is NUL-terminated (it was duplicated
via `strndup_checked`).

### A.4 `push_scope` and `push_tag_scope`

```c
static VarScope *push_scope(char *name) {
  VarScope *vs = calloc_checked(1, sizeof(VarScope));
  vs->name = name;
  vs->next = scope->vars;
  scope->vars = vs;
  return vs;
}
```

Caller fills in `var`, `type_def`, or `enum_*` after.  The
returned pointer is mutable until the next `push_scope` on the
same scope (which prepends in front).

`push_tag_scope(tok, ty)` analogous on the tag list, taking the
tag identifier token + the resolved type.

### A.5 `new_unique_name`

```c
static char *new_unique_name(void) {
  return format(".L..%d", label_cnt++);
}
```

Used for compiler-emitted label names (control-flow continuation
labels, switch break labels, goto unique labels).  The `.L..`
prefix marks them as macOS local symbols (stripped during
linking).  `label_cnt` is process-wide — same caveat as
`04_parse.md` §3 / §J.3 of `04a_decl.md`.

Distinct from the `_cg_` prefix used for `&&label` (per `04b_
expr.md` §G.9), which must survive linking.

---

## B. `compound_stmt`

The block statement: `{ … }`.  Pushes a scope on `{`, pops on
`}`.

```
compound-stmt := "{" __label__-decls? (declaration | stmt | _Static_assert)* "}"
```

### B.1 Setup

1. `tok = skip(tok, "{")`.
2. `enter_scope()`.
3. Initialize an empty result chain.

### B.2 `__label__` declarations (GCC extension)

A leading `__label__ name1, name2;` declares local labels (per
GCC: labels whose scope is the block, distinct from
function-scope labels).

ncc's behavior: parse and discard the names.  Labels emitted by
`stmt` (§E.4) are placed in the function-wide `labels` chain
without per-block scoping; `__label__` is currently a no-op.

The parser handles `__label__` in two positions:
- Top of block (before any statements): leading-loop.
- Anywhere inside the block: handled at the start of the body
  loop (continues to the next iteration).

Both forms simply consume the keyword and the
comma-separated identifier list, ending at `;`.

### B.3 Body loop

Loop while not at `}`:

1. **`__label__`** mid-block: parse and discard (per §B.2).
   Continue.
2. **`_Static_assert` / `static_assert`**: parse the test
   expression via `const_expr_val`, optional message string,
   close paren, semicolon.  Unlike file-scope `_Static_assert`
   (`04a_decl.md` §I.2), the in-block form does **not** check
   the value — block-scope `_Static_assert` failures are
   silently dropped.  This is a known divergence from C11
   strictness; flagged in §I below.
3. **Declaration** (`is_typename(tok) && next != ":"`):
   - Parse declspec into `basety` + `attr`.
   - If `attr.is_typedef`: dispatch `parse_typedef` (§B.5).
   - Else if inside a function and the declarator looks like a
     function definition (`is_function_definition` lookahead):
     parse declarator, dispatch `nested_function` (§F.4).
   - Otherwise: `cur = cur->next = declaration(...)`.

   The `next != ":"` guard distinguishes a labeled statement
   (`name:`) from a declaration starting with a typedef name —
   `is_typename` would return true for a typedef-named label,
   but the colon disambiguates.
4. **Statement**: `cur = cur->next = stmt(...)`.

After the loop, `leave_scope()`, set `node->body = head.next`,
return.

### B.4 `is_function_definition`

Lookahead helper used by `compound_stmt` to detect nested
function definitions.  Logic:

1. If next token is `;`, not a function definition.
2. Try-parse a `declarator(dummy, tok, copy_type(basety))`.
3. If the resulting type is `TY_FUNC` and the next token after
   the declarator is `{`, it's a function definition.

This is a pure lookahead — the actual parse is redone in
`nested_function`.

### B.5 `parse_typedef`

Comma-separated list of declarators after a typedef.  For each:
1. Parse declarator against `basety`.
2. Consume trailing `__attribute__((...))`.
3. Require name; error otherwise.
4. `push_scope(name)->type_def = ty`.
5. If a `cur` chain pointer was passed (i.e., we're inside a
   block), call `compute_vla_size(ty, ty->name)` and append the
   resulting size statement to the chain.  This handles VLA
   typedefs: `typedef int A[n];` emits the size computation at
   the typedef's lexical position.

`parse_typedef` is reachable from both top-level (`parse`,
`04a_decl.md` §I.2) and `compound_stmt` (§B.3).

---

## C. Selection statements

### C.1 `if` / `else`

Build `ND_IF` with `cond` (parsed as `expr` between parens),
`then`, optional `els`.

**Dead-code elimination:** if `cond` folds to a compile-time
integer constant **and** we are not inside a switch **and** the
dead branch contains no `goto` labels:

- `if (0) then-stmt`: replace with `els` (or empty block if no
  `els`).
- `if (nonzero) then-stmt else els-stmt`: replace with `then`.

The `!current_switch` guard is critical: case labels inside `if
(0) { case X: ... }` are still reachable through the surrounding
switch — eliminating the branch would lose them.

The `contains_label` recursion walks the dead branch looking for
`ND_LABEL` nodes; if any is found, the branch is preserved
because forward `goto` could reach it.

This DCE is one of the few parser-side optimizations in ncc.
Codegen does additional folding, but this removes whole
statement subtrees rather than just narrow constants.

The dangling-else rule binds to the nearest `if` per C11
§6.8.4/3, achieved by the recursive `stmt(...)` call for `then`
and `els`.  Because `stmt` recurses into `if/else`, a stray
`else` consumes the inner `if`, producing the standard nesting.

No scope is pushed around the controlled statements; if the
controlled statement is `{...}`, that block has its own scope
via `compound_stmt`.

### C.2 `switch`

1. Build `ND_SWITCH` with `cond` (parsed as `expr` between
   parens).
2. Save and update `current_switch` (so `case` and `default`
   can register against this switch).
3. Save and replace `brk_label` with the switch's
   `unique_label` (so `break` resolves to switch exit).
4. Parse the body via `stmt(rest, tok)` into `then`.
5. Restore `current_switch` and `brk_label`.

Note: `cont_label` is **not** touched.  A `continue` inside a
switch refers to the enclosing loop, not the switch — matches
C semantics (§6.8.6.2/1).

The case list is built up by `ND_CASE` registrations during the
body parse (§C.3), each prepended to `current_switch->case_
next`.

### C.3 `case` and `default`

Both produce `ND_CASE` nodes (the same kind for both regular
case labels and `default`).

**`case CONST [...CONST] :`**:
1. Error if not inside a switch.
2. Parse `begin = const_expr_val(...)`.
3. If next token is `...` (GCC range extension), parse `end =
   const_expr_val(...)`; else `end = begin`.
4. Skip `:`.
5. `node->label = new_unique_name()` (the per-case dispatch
   target).
6. Parse the controlled statement via `stmt`.
7. Prepend to `current_switch->case_next`.
8. Set `node->begin`, `node->end`.

The case range (`begin..end` inclusive) gives codegen a span to
match.  Codegen emits a single test per case (or a jump table
for dense cases).

**`default :`**:
1. Error if not inside a switch.
2. Skip `:`.
3. `node->label = new_unique_name()`.
4. Parse the controlled statement.
5. `current_switch->default_case = node`.

C semantics: at most one `default` per switch.  ncc currently
does not enforce uniqueness — a second `default` overwrites the
first via the simple assignment.  This is benign in practice
(the program is malformed anyway) but a known leniency.

---

## D. Iteration statements

All three iteration forms produce `ND_FOR` (the do-while uses
`ND_DO` to flip the condition test order in codegen).  All three
manage `brk_label` and `cont_label`:

- `brk_label` is set to the loop's `unique_label`.
- `cont_label` is set to a fresh `new_unique_name`.
- Both are saved and restored around the body parse.

### D.1 `while (cond) stmt`

```
ND_FOR { cond = expr, then = stmt, unique_label, cont_label }
```

No scope pushed.  No init or inc.

### D.2 `do stmt while (cond);`

`ND_DO` with `then` parsed before `cond`.  The semicolon after
`)` is consumed.  No scope pushed.

### D.3 `for (init; cond; step) stmt`

1. **Push scope.**  This scope spans the entire `for` because
   init may declare a variable (C99 init-decl) whose scope must
   include `cond`, `step`, and the body.
2. Save `brk_label` / `cont_label`; install fresh ones.
3. **Init**:
   - If `is_typename(tok)`: parse `declspec` + `declaration`.
   - Else: parse `expr_stmt`.
4. **Cond**: optional, parsed as `expr` if not `;`.  Default
   missing-cond is "always true" (codegen handles missing cond
   as a non-branching loop-top).
5. **Step**: optional, parsed as `expr` if not `)`.
6. **Body**: parsed as `stmt`.
7. **Pop scope.**
8. Restore `brk_label` / `cont_label`.

The scope-push is the only iteration form that needs one (the
init declaration's name must be visible in cond/step/body).

### D.4 Break/continue target stack

The `brk_label` / `cont_label` are **not** stacked explicitly —
each iteration construct saves the current values to local
variables, installs new ones, parses the body (during which
`break` / `continue` snapshot the inner labels), then restores.

This is morally a stack but spread across multiple stack frames.
Nesting works because of the save/restore around body parses.

`brk_label` is shared between switches and loops; the
innermost-applicable wins for `break`.  `cont_label` is set
**only** by loops, never by switches — `continue` inside a
switch reaches past it to the enclosing loop, which is the
correct C behavior.

---

## E. Jump statements and labels

### E.1 `goto`

Two forms:

**Direct: `goto IDENT;`**
- Build `ND_GOTO` with `label = strndup(...)` of the identifier
  text.
- Prepend to `gotos` chain.
- The `unique_label` is filled in at end-of-function by walking
  `labels` and matching by name.

**Computed: `goto * expr;`**
- Build `ND_GOTO_EXPR` with `lhs = expr(...)`.
- No name resolution; codegen materializes a register-indirect
  branch.
- Used with labels-as-values (`&&label`) — the parser doesn't
  link `ND_GOTO_EXPR` to `ND_LABEL_VAL` Nodes; the connection
  is via runtime address values.

### E.2 `break`

Build `ND_GOTO` with `unique_label = brk_label`.  No `label`
field (it's resolved already to the unique name).

Error "stray break" if `brk_label` is NULL (no enclosing
loop/switch).

### E.3 `continue`

Build `ND_GOTO` with `unique_label = cont_label`.

Error "stray continue" if `cont_label` is NULL (no enclosing
loop).

### E.4 Labeled statement

`IDENT :` followed by a statement (the `tok->kind == TK_IDENT
&& equal(tok->next, ":")` branch).

1. Build `ND_LABEL` with `label = strndup(...)` of the
   identifier and `unique_label = new_unique_name()`.
2. Skip past `IDENT :`.
3. Consume any trailing `__attribute__((...))` (GCC permits
   labels to bear attributes; ncc parses and discards).
4. Parse the controlled statement via `stmt`.
5. Prepend to `labels` chain.

Distinct from `__label__` (§B.2): this declares a label whose
scope is function-wide.

### E.5 Label resolution

Performed at end-of-function (in `function`, §F.3):

```
for each g in gotos:
  if g->kind == ND_LABEL_VAL: skip (handled separately)
  for each l in labels:
    if g->label == l->label:
      g->unique_label = l->unique_label
      break
  if g->unique_label still NULL:
    error "use of undeclared label"
```

For `ND_LABEL_VAL` (`&&label`), the unique_label is allocated
at the *use* site (per `04b_expr.md` §G.9), so the resolution
walks `gotos` looking for matching `labels` and copies
`g->unique_label → l->unique_label`.  The order matters: the
`&&label` site needs a name that survives linking (`_cg_N`),
and the matching `ND_LABEL` then borrows it.

### E.6 ASM statement

`asm` / `__asm__` followed by `(...)`.  Optional qualifiers
(`volatile`, `__volatile__`, `inline`, `goto`) consumed without
effect.

The body is the GCC extended-asm syntax:
```
asm ( template
    : output-operands
    : input-operands
    : clobbers
    : goto-labels )
```

Each `:`-delimited section is optional; trailing sections are
consumed but only outputs/inputs/clobbers/goto-labels are
recorded.

**Template**: one or more adjacent string literals,
concatenated.  Stored in `node->asm_str`.

**Output operands**: comma-separated `[name]? "constraint"
(expr)`.  Each operand contributes a constraint string, an
expression, and an optional name.  Up to 16 operands per side
(static-array bound).  Constraint strings may themselves be
adjacent-literal-concatenated (e.g. `__stringify(c) "r"`).
Expressions are parsed via `expr` and given to `add_type`.

**Input operands**: same shape, parsed identically.  Stored in
`asm_input_*` arrays.

**Clobbers**: comma-separated string literals.  Stored in
`asm_clobbers`.

**Goto labels** (asm goto): comma-separated identifiers, stored
in `asm_goto_labels`.

The 16-element bound is a hard cap; exceeding it errors.  Real-
world Linux kernel code has plenty of asm with many operands;
the bound is sized for the test corpus and may need lifting for
serious kernel work.  Flagged in §I below.

After the operand parse, the closing `)` and `;` are consumed.

### E.7 Expression statement (`expr_stmt`)

Standalone expression followed by `;`:
- Empty `;` → `ND_BLOCK` with empty body (so codegen emits no
  instructions).
- Otherwise → `ND_EXPR_STMT` with `lhs = expr(...)`.

`expr_stmt` is the fall-through case in `stmt`'s dispatch — any
input that isn't a keyword-led statement, labeled statement, or
block is treated as an expression statement.

---

## F. Function definitions

`function(Token *tok, Type *fn_ty, VarAttr *attr)` is reached
from top-level `parse` (`04a_decl.md` §I.2 — when a top-level
declarator is followed by `{` or by a type-keyword for K&R).

### F.1 Setup

1. Look up any prior declaration of the function name.  If
   found and `is_function`, save its `align` (preserve from
   prior `__attribute__((aligned(N)))`).
2. Create `Obj` via `new_gvar(name, fn_ty)`; mark
   `is_function = true`, restore `prev_align` if larger,
   `is_definition = true`.  Copy `is_static`, `is_extern`,
   `is_inline`, `is_variadic` from `attr` and `fn_ty`.
3. Save current per-function state on the C stack (`current_fn`,
   `locals`, `gotos`, `labels`, `brk_label`, `cont_label`,
   `current_switch`).  Clear them.
4. `enter_scope()`.

### F.2 Parameters

Process parameters in reverse so that `new_lvar` (which
prepends to `locals`) yields them in declaration order.

For each parameter:
- If unnamed (`!name || name->len == 0`), generate via
  `new_unique_name()`.
- Otherwise `strndup_checked(name->loc, name->len)`.
- `new_lvar(pname, param_ty)` registers in scope and `locals`.

After the loop, `fn->params = locals`.

### F.3 K&R-style declarators

If the next token is not `{`, K&R-style parameter declarators
are interleaved between `)` and `{`.  Parse loop:

1. Parse `declspec`.
2. Comma-separated declarators within this declaration.
3. For each declarator:
   - Look up the parameter name in `fn->params`.
   - If found, update its type and align.
   - Also update the corresponding entry in `fn_ty->params` so
     the function type reflects the declared signature.

K&R declarators that don't match a previously-declared parameter
name → error "parameter name omitted in K&R declaration"
(strict: ncc rejects extra K&R declarators).

### F.4 Variadic and alloca infrastructure

If `fn_ty->is_variadic`:
- Create `__va_area__` local of `pointer_to(ty_char)` with
  align 8, store on `fn->va_area`.

Always:
- Create `__alloca_bottom__` local of `pointer_to(ty_char)`,
  store on `fn->alloca_bottom`.

These are placeholders codegen reads to set up variadic arg
access and alloca tracking.

### F.5 Body

Parse via `compound_stmt`.  The result `Node *` is the function
body.

Then run `add_type(body)` to populate every Node's `ty`.

Resolve labels (§E.5).

Set `fn->body`, `fn->locals = locals`.

`leave_scope()`.

Restore the saved per-function state.

### F.6 Variadic VLA-parameter side effect re-evaluation

(Cross-link: per `04a_decl.md` §E.4, VLA parameter dimensions
have their `vla_dim_tok` preserved through the array-to-pointer
decay.  `function` is the consumer of that: at the start of the
body, before parsing user code, the parser walks parameters
that retain `vla_dim_tok`, re-parses the dimension expression
in the body's scope, and emits its side-effect statements at
the top of the body.

This handles `void f(int n, int a[n++])` — the `n++` must
execute as the function is entered, not just be ignored.)

This re-evaluation flow is implemented via a per-parameter
`vla_dim_tok` walk + emission of `ND_EXPR_STMT` Nodes prepended
to the body chain.  See parse.c around lines 5750–5800 for the
exact emission point; the contract is "side effects in VLA
parameter dimensions execute at function entry, in declaration
order."

---

## G. GNU statement expression

`( { stmt-or-decl* } )` — covered in `04b_expr.md` §H.4
(primary's first branch in the `(` family).

The implementation: parse a `compound_stmt` whose body becomes
the `ND_STMT_EXPR`'s body chain.  Pushes its own scope (per
§B).

The value of the statement expression is the value of the last
expression statement, accessed by walking to the tail of `body`.
Codegen handles this; the parser emits the body verbatim.

---

## H. Local labels (`__label__`)

Per §B.2, `__label__ name1, name2;` at the top of a block
declares local labels.  ncc parses and discards the names —
labels are currently function-scoped regardless of declaration.

This is a known incompleteness: GCC's local-label semantics
limit the label's scope to the block, allowing the same label
name to be reused in sibling blocks.  ncc treats all labels as
function-scoped, so reusing a label across blocks errors.

The test corpus does not exercise this; flagged in §I.

---

## I. Open questions

### I.1 Block-scope `_Static_assert` is silent

Per §B.3, in-block `_Static_assert` parses but does not check
the value.  C11 requires the test to be evaluated and a
diagnostic emitted on failure.  `main` matches this leniency.

Disposition: document the divergence in §13 of `04_parse.md`.
Implementation can be added trivially during Phase 4 if
desired.

### I.2 Switch `default` uniqueness not enforced

Per §C.3, a second `default` overwrites the first.  C11 requires
a diagnostic.

Disposition: low priority; unclean source code anyway.  Document
in §13 if desired, but not blocking.

### I.3 Hard 16-operand cap on inline asm

Per §E.6, output/input/clobber/goto-label lists are capped at
16 entries each via static arrays.  Real Linux kernel asm
exceeds this in some places.

Disposition: lift the cap during Phase 4 impl by switching to a
linked-list accumulator (or a dynamic array).  This is a
straightforward implementation change; the spec contract just
documents that the cap should not exist.

### I.4 `__label__` is parsed-but-ignored

Per §H, GCC's local-label semantics aren't implemented.  Most
real-world code doesn't rely on label scoping; the few cases
that do produce errors when the same label is reused in sibling
blocks.

Disposition: defer.  Implementing it requires a per-block
label namespace, modest complexity.  If the test corpus
flags a regression, escalate.

---

## J. Function definitions (top-level vs nested)

The `compound_stmt` body loop dispatches to **`nested_function`**
when `is_function_definition` returns true and we are inside a
function.  Otherwise top-level `function` runs (from `parse`).
The two share most of the structure but differ in capture
handling.

### J.1 Top-level function (`function`)

Already covered in §F.

### J.2 Nested function (`nested_function`)

GCC nested functions (per `04_parse.md` §9):

```c
int outer(int x) {
  int inner(int y) { return x + y; }
  return inner(5);
}
```

The `inner` function references `x` from `outer`'s frame.  ncc
implements this via:

1. **Mangled name**: `inner` becomes `outer.inner` in the
   symbol table to avoid collision with other `inner`s.
2. **Chain pointer**: `inner` gets a hidden first parameter
   `__chain__` of type `char *` holding the outer's frame
   address.
3. **Captured variable rewrite** (`rewrite_nested_var_refs`):
   every `ND_VAR` reference to an outer local is rewritten to
   `ND_CHAIN_VAR` which codegen lowers to a load through the
   chain pointer.

### J.3 `nested_function` flow

1. Save outer state (`current_fn`, `locals`, `gotos`, `labels`,
   `brk_label`, `cont_label`, `current_switch`).
2. **Set `saved_fn->locals = saved_locals`** before recursing.
   Multi-level nesting needs to walk the enclosing chain;
   `saved_fn` has not yet had `locals` finalized at this point,
   so this temporary assignment makes the chain walkable.
3. Create `new_gvar(mangled_name, fn_ty)` with `is_function`,
   `is_definition`, `is_static`, `is_variadic`, `is_nested =
   true`, `enclosing_fn = saved_fn`.
4. Reset per-function state.
5. `enter_scope()`.
6. Allocate `chain_param`: a hidden `__chain__` local of
   `pointer_to(ty_char)`.  Stored on `fn->chain_param`.
7. Process declared parameters (same reverse-order pattern as
   `function`).
8. Handle K&R-style param declarators if present (same as
   `function`).
9. Allocate `__va_area__` (if variadic) and `__alloca_bottom__`.
10. Push `inner_name` (un-mangled) into the scope so recursive
    calls inside `inner` resolve.
11. Parse body via `compound_stmt`.

### J.4 Label resolution within nested function

After body parse:

1. **First pass**: for each `ND_LABEL_VAL` in the inner's
   `gotos`, copy its `unique_label` into matching `ND_LABEL`'s
   `unique_label`.  This allows `&&label` and the label to
   share the symbol name (per `04b_expr.md` §G.9).
2. **Second pass**: for each `ND_GOTO` in `gotos`, find the
   matching label by name; copy `unique_label`.

### J.5 Non-local goto via longjmp

Unresolved `ND_GOTO` nodes (no matching label inside the inner
function) are presumed to target a label in an enclosing
function.  ncc lowers these via setjmp/longjmp:

For each unresolved `ND_GOTO`:
1. Allocate a `__nlgoto_<label>` array of 3 `pointer_to
   (ty_char)` slots in the **outer** function's locals (this
   is why `saved_locals` is mutated in step 2 of J.3 — the
   buffer is added to the outer's local list).  Align 8.
2. Set `g->nlgoto_buf` to the buffer.
3. Set `g->lhs` to a `new_var_node(buf)`.

Codegen emits `setjmp(__nlgoto_<label>)` at the matching
`ND_LABEL` in the outer function and `longjmp(__nlgoto_
<label>, 1)` at the inner's `ND_GOTO`.

### J.6 Captured variable rewrite (`rewrite_nested_var_refs`)

Walk the inner's body AST.  For each `ND_VAR` referencing a
local (`var->is_local`):

1. If the variable is in the **immediate** outer's locals (one
   level up): rewrite to `ND_CHAIN_VAR`.  Set `node->lhs =
   new_var_node(chain_param)` (the chain into the outer's
   frame), preserve `node->var = orig_var` and `node->ty =
   orig_var->ty`.

2. If the variable is in a **deeper** enclosing function (two+
   levels up): walk the `enclosing_fn` chain, accumulating each
   level's `chain_param` into `chain_path[]`.  Maximum depth 4.
   Once the variable is found, rewrite to `ND_CHAIN_VAR` with
   `chain_depth` and `chain_path[]` populated so codegen knows
   how many chain-loads to perform.

The walk is recursive, descending into all child Node fields
(`lhs`, `rhs`, `cond`, `then`, `els`, `init`, `inc`, `body`,
`args`, `asm_input/output_exprs`, `cas_*`).  Skips Nodes where
`var->is_local` is false — globals are not captured.

### J.7 Propagating unresolved nodes upward

After the inner's body is processed:
- Collect `ND_LABEL_VAL` Nodes whose label was not resolved
  inside the inner — these target a label in some enclosing
  function.  Copy each to a new Node and add to the
  unresolved chain.
- Collect non-local `ND_GOTO` Nodes (those with `nlgoto_buf`).
- Prepend the unresolved chain to the outer's `gotos` for
  resolution at the outer's end-of-function.

### J.8 Cleanup

1. `fn->locals = locals`; `leave_scope()`.
2. Restore outer state.
3. Append unresolved nodes to outer's `gotos`.
4. Push a scope binding for `inner_name → fn` in the outer's
   scope (so calls to `inner` from outer resolve).
5. Append a `ND_NULL_EXPR` to the outer's statement chain in
   place of the nested-function definition (the function itself
   is emitted via the `Obj` chain; the statement-level slot is
   a no-op).

---

## K. `Obj` field population by 04c

| Field | Set by |
|---|---|
| `params` | `function`, `nested_function` |
| `body` | `function`, `nested_function` |
| `locals` | `function`, `nested_function` (via `fn->locals = locals` after body parse) |
| `va_area` | `function`, `nested_function` if `is_variadic` |
| `alloca_bottom` | `function`, `nested_function` |
| `is_nested` | `nested_function` (true) |
| `chain_param` | `nested_function` |
| `enclosing_fn` | `nested_function` (= `saved_fn`) |
| `nlgoto_targets` | (codegen-side; declared but not populated by parser) |
| `is_definition` | `function` (true), `nested_function` (true) |
| `is_static` | `nested_function` (always true), `function` from `attr->is_static` |
| `is_inline` | `function` from `attr->is_inline` |
| `is_variadic` | `function`, `nested_function` from `fn_ty->is_variadic` |

The nested-function `is_static = true` is significant: nested
functions are always emitted as static symbols (they have no
external linkage by C semantics).  The mangled name is
`outer.inner` to avoid clashes; combined with `is_static` this
gives correct symbol scoping.

---

## L. Worked examples

### L.1 `for (int i = 0; i < n; i++) body;`

1. Push scope.
2. Save brk/cont, install fresh.
3. `is_typename(int)` → true.  Parse declspec (int) +
   declaration: emits `int i = 0;` as a body that prepends to
   `node->init`.
4. Parse cond `i < n` (the scope sees `i`).
5. Parse step `i++`.
6. Parse body via `stmt`.
7. Pop scope.
8. Restore brk/cont.

Resulting `ND_FOR`:
- `init = ND_BLOCK { i = 0 }`.
- `cond = ND_LT(i, n)`.
- `inc = (post-inc lowering of i++)`.
- `then = body`.
- `unique_label`, `cont_label` populated.

### L.2 `switch (x) { case 1: a(); break; default: b(); }`

1. Build `ND_SWITCH`, parse `cond = x`.
2. `current_switch = node`, `brk_label = node->unique_label`.
3. Parse body — a compound_stmt containing two case branches:
   - `case 1:` → `ND_CASE` with `begin=1, end=1, label=L0`,
     `lhs = ND_BLOCK { a(); break; }`.  Prepend to
     `current_switch->case_next`.
   - `default:` → `ND_CASE` with `label=L1`, `lhs = ND_BLOCK {
     b(); }`.  Set `current_switch->default_case`.
4. `break;` inside body resolves to `brk_label = node->unique_
   label`.

### L.3 Forward goto

```c
void f(void) {
  goto L;
  L:;
}
```

1. `goto L;` → `ND_GOTO { label = "L", goto_next = gotos }`.
   Prepended to `gotos`.
2. `L:;` → `ND_LABEL { label = "L", unique_label = ".L..0" }`.
   Prepended to `labels`.
3. End of function: walk `gotos`, find matching `labels` entry,
   copy `unique_label` into goto's `unique_label`.

### L.4 Nested function with capture

```c
int outer(int x) {
  int inner(int y) { return x + y; }
  return inner(5);
}
```

1. `outer` parses normally to `function`.  Inside its body,
   `compound_stmt` sees `int inner(int y)` followed by `{` —
   `is_function_definition` returns true, dispatches to
   `nested_function`.
2. `nested_function`:
   - Mangled name `outer.inner`.
   - `fn->is_nested = true`, `fn->enclosing_fn = outer`.
   - `chain_param = __chain__` (`char *`).
   - Body parsed; `x + y` becomes `ND_ADD(ND_VAR(x), ND_VAR
     (y))`.
3. `rewrite_nested_var_refs(fn->body, &temp_outer)`:
   - `ND_VAR(x)` matched against `outer->locals` → rewritten
     to `ND_CHAIN_VAR { lhs = new_var_node(chain_param), var =
     x_obj, ty = ty_int }`.
   - `ND_VAR(y)` references inner's own param → not rewritten.
4. `outer.inner` registered in outer's scope as `inner`.
5. `inner(5)` resolves via funcall (`04b_expr.md` §H.5).
   Because `inner.is_nested`, `funcall` appends a chain
   argument `ND_FRAME_ADDR` (outer is calling its direct
   child, so passes its own fp).
6. Codegen:
   - `outer.inner` reads `chain_param` from its first arg slot.
   - On reading `x`: load through `chain_param` at the offset
     of `x` in outer's frame.

### L.5 Computed goto via labels-as-values

```c
void *targets[] = { &&L1, &&L2 };
i = 0;
goto *targets[i];
L1: ...;
L2: ...;
```

1. Initializer parses two `&&Label` expressions.  Each builds
   `ND_LABEL_VAL` with `unique_label = "_cg_0"` / `"_cg_1"`,
   prepended to `gotos`.
2. `goto *targets[i];` → `ND_GOTO_EXPR { lhs = expr }`.
3. `L1:` → `ND_LABEL { label = "L1", unique_label = ".L..0" }`.
4. End-of-function resolution:
   - Iterate `gotos`.  For each `ND_LABEL_VAL` (`L1`,
     `L2`), find matching `ND_LABEL` and copy
     `g->unique_label → l->unique_label` so `L1`'s label
     emission uses `_cg_0`.
   - For `ND_GOTO_EXPR`, no resolution needed (the address
     comes from the expression).

---

## M. Phase 5 prerequisites added by 04c

Append to `04_parse.md` §15 master list during impl review:

1. **`brk_label` / `cont_label` are baked into Node identity.**
   `ND_GOTO` nodes built by `break` / `continue` already hold
   the `unique_label`; codegen materializes a branch to that
   label.

2. **`ND_SWITCH.case_next` and `ND_SWITCH.default_case`.**
   Codegen iterates `case_next` (LIFO order from registration)
   and emits dispatch.  Each `ND_CASE` has `begin`, `end`,
   `label`, `lhs` (the controlled statement).

3. **`ND_LABEL` is the materialization point.**  `unique_label`
   becomes the emitted symbol name.  `label` (user-visible
   name) is used for diagnostics only.

4. **`ND_DO` semantics differ from `ND_FOR`.**  `ND_DO` tests
   condition after body; `ND_FOR` tests before.

5. **`ND_ASM` carries operand arrays.**  Codegen substitutes
   constraint-driven register/memory operands into the asm
   template.

6. **`ND_CHAIN_VAR` carries chain depth.**  Codegen issues
   `chain_depth` chain-loads from the current `__chain__`
   parameter to reach the target frame, then loads the variable
   at its offset within that frame.

7. **`ND_FRAME_ADDR` materializes the current function's
   frame pointer.**  Used at nested-call sites to pass the
   chain.

8. **`Obj.nlgoto_targets`** is consumed by codegen to emit
   `setjmp` at non-local goto target labels.  Population is
   currently codegen-side; the parser flags the buffer via
   `g->nlgoto_buf` on each non-local `ND_GOTO`.

9. **VLA-parameter side-effect re-evaluation** (§F.6) emits
   `ND_EXPR_STMT` Nodes at the top of `fn->body`.  Codegen
   sees them as ordinary statements, no special handling.

---

## N. Cross-references

- Local declarations: `04a_decl.md` §H.
- Function-definition entry: `04a_decl.md` §I.5.
- Variable creation helpers (`new_lvar`): `04a_decl.md` §J.2.
- Statement expression and the `({…})` form: `04b_expr.md`
  §H.4.
- Labels-as-values (`&&label`): `04b_expr.md` §G.9.
- Nested function chain-pointer call site: `04b_expr.md` §H.5.
- Cross-cutting scope model: `04_parse.md` §4.
- Cross-cutting nested-function decision: `04_parse.md` §9.
