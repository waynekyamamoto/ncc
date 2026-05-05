# ncc Parser — Statements & Functions Sub-Chunk (Phase 4, 04c)

This sub-chunk specifies the **statement zone** of `src/parse.c`:
control-flow statements, labels, function definition bodies,
local declarations interleaved with statements, and the scope
mechanics those rely on.

Cross-cutting decisions (scope namespaces, `is_typename`, etc.)
live in `04_parse.md`; this file cites them.

**Coverage in `src/parse.c` (per `04_parse_inventory.md`):**

| Lines | Section |
|---|---|
| 6–113 | Scope management (`Scope`, `enter_scope`, `leave_scope`, `find_var`, `find_tag`) |
| 32–50 | Break/continue target tracking |
| 51–72 | Nested function captured-variable support |
| 109–113 | Label name minting |
| 4019–4546 | Statement parsing (compound, if, while, for, do, switch, case, default, goto, label, break, continue, return, expression statement) |

Local declaration parsing (interleaved with statements inside a
`compound_stmt` body) lives in `04a_decl.md` §H.  This sub-chunk
specifies the statement-list construction and how local decls are
spliced in.

**Status:** skeleton.

---

## A. Scope mechanics

### A.1 `Scope` data structure

[Field-by-field reference: vars (ordinary namespace), tags (tag
namespace), parent pointer (chain), depth.  How the linked list
is maintained.]

### A.2 `enter_scope` / `leave_scope`

[Push and pop.  Where they are called (per §4.3 of `04_parse.md`).
Invariants: no leaks (every `enter_` matches a `leave_`); no
re-pop (one-shot).]

### A.3 Symbol lookup

[`find_var(tok)` walks scope chain inward to outward.  Returns
the first match or NULL.  `find_tag(tok)` likewise on the tag
chain.  Typedef names are in the ordinary namespace (per
`04_parse.md` §4.1).]

### A.4 Label namespace (per-function)

[Labels are not in the lexical scope chain.  Each function has a
flat label table.  Forward `goto` to a label later in the
function is permitted; forward references are recorded and
resolved at end-of-function.]

## B. `compound_stmt`

[The block statement: `{ ... }`.  Parses a sequence of statements
and local declarations, in any order.  Builds the result Node
list incrementally.  Pushes a scope on `{`, pops on `}`.]

### B.1 Declaration vs statement disambiguation

[At each iteration: `is_typename(tok)` decides declaration vs
statement.  Special case: a label followed by either a
declaration or a statement.]

### B.2 VLA size-statement insertion

[Per `04_parse.md` §7.3, when a local declaration contains a VLA,
the size-computation statement is appended to the result list
*before* the declaration's own initialization statements.  This
sub-chunk's `compound_stmt` is the buffer.]

## C. Selection statements

### C.1 `if` / `else`

[`if (cond) then-stmt`, `if (cond) then-stmt else else-stmt`.
Dangling-else binds to nearest `if`.  No scope pushed; if the
controlled statement is `{ ... }`, that's a compound_stmt with
its own scope.]

### C.2 `switch`

[`switch (cond) stmt`.  Pushes a switch context (for `case` /
`default` matching).  Tracks per-case constant values; emits a
dispatch table-or-chain (codegen's choice).  `default` is the
fallthrough.]

### C.3 `case` and `default`

[Inside a switch.  `case CONST_EXPR :` — `CONST_EXPR` folded via
`eval_node`.  GCC range form `case A ... B :` recognized.
`default :` — at most one per switch.]

## D. Iteration statements

### D.1 `while`

[`while (cond) stmt`.]

### D.2 `do ... while`

[`do stmt while (cond);`.]

### D.3 `for`

[`for (init; cond; step) stmt`.  Init may be a declaration; if
so, pushes a scope spanning the entire `for`.  All three of
init/cond/step are optional.]

### D.4 Break and continue targets

[Stack of `(break_label, continue_label)` pairs maintained as
loops + switches are entered.  `break` inside switch refers to
the switch's break_label; `continue` inside switch ignores switch
and refers to the enclosing loop (per C semantics).]

## E. Jump statements

### E.1 `goto`

[`goto IDENT;` — direct.  `goto *expr;` — computed (labels-as-
values).  Direct gotos are recorded for end-of-function
resolution.  Computed gotos produce `ND_GOTO_EXPR` Node kind.]

### E.2 `break` / `continue`

[Resolve to the top-of-stack target per §D.4.  Error if no
enclosing loop/switch.]

### E.3 `return`

[`return;` (void function) and `return expr;`.  `expr` is implicitly
cast to the return type per `usual_arith_conv`.  Type-system §7
does the cast insertion.]

## F. Function definitions

[Top-level dispatch from `parse` (`04a_decl.md` §I) lands here
when a declarator is followed by `{`.]

### F.1 Parameter binding

[Pre-body: each parameter type from the declarator is converted
to an `Obj` with stack-storage; pushed into the function's
locals list.  Parameter names enter the ordinary namespace of
the function-body scope.]

### F.2 Body parsing

[Parse the `compound_stmt`.  At end, run `add_type` over the body
to populate `Node->ty`.  Run a falloff check: non-void function
without a final `return` is permitted but produces an implicit
return-zero (matches main; verify).]

### F.3 Label resolution

[After body parse, all forward `goto` targets are matched to
emitted labels.  Unresolved → error.]

### F.4 Nested function definitions

[Per `04_parse.md` §9.  When a function definition is parsed
inside another function's body, the inner function's `Obj` is
linked into a separate "nested fns of outer" list and emitted at
end-of-translation-unit alongside the outer.  Captured variables
from the outer scope are recorded in the inner's `outer_vars`
list.]

### F.5 Variadic functions and `va_list`

[`...` in the parameter list sets `Obj->ty->is_variadic`.  The
parser does not generate va_list machinery here; codegen does
(per Phase 5).  But the parser must surface the
trigger condition for the `e7e7393` divergence-log fix —
specifically, what `va_start` looks like when there's no preceding
named argument.  Parse-side recording per `04b_expr.md` §M.]

## G. Statement expressions

[`({ stmt; stmt; expr; })` GCC extension.  Pushes a scope, parses
a `compound_stmt`, the value is the final expression's result.
The block must end in an expression statement (or be empty, in
which case the value is void).]

## H. Local labels

[`__label__ name;` GCC extension at the top of a block declares
`name` as a local label whose scope is just that block.  Distinct
from function-scope labels.  Implemented via a per-block label
namespace (small variant of §A.4).]

## I. Worked examples

### I.1 `for (int i = 0; i < n; i++) ...`

[Init-as-declaration scope mechanics.]

### I.2 `switch + case + fallthrough`

[Case-list construction; break_label resolution.]

### I.3 Forward `goto`

[Label resolution at end-of-function.]

### I.4 Nested function with capture

[`int add(int x) { int inner(int y) { return x + y; } return
inner(5); }`.  Capture of `x`; `outer_vars` population.]

## J. Phase 5 prerequisites added by 04c

[AST invariants from this sub-chunk; append to `04_parse.md` §15
during impl review.]

## K. Open questions

[Sub-chunk-specific questions raised during drafting.]
