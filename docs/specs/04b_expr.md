# ncc Parser — Expressions Sub-Chunk (Phase 4, 04b)

This sub-chunk specifies the **expression zone** of `src/parse.c`:
the precedence ladder, primary expressions, postfix operators,
unary operators, the cast/sizeof/alignof/_Generic family,
constant expression evaluation (`eval_node` / `try_eval_node`),
and the parse-time complex / vector lowering.

Cross-cutting decisions live in `04_parse.md`; this file cites
them.

**Coverage in `src/parse.c` (per `04_parse_inventory.md`):**

| Lines | Section |
|---|---|
| 115–163 | Node construction helpers (`new_node`, `new_unary`, `new_binary`, `new_num`, `new_var_node`) |
| 164–399 | Complex / vector helpers (`__real__` / `__imag__` constructors, vector element extract/set) |
| 1806–1819 | Expression parsing entry (`expr`) |
| 1820–2284 | Constant expression evaluation (`eval_node`, `try_eval_node`, complex evaluator) |
| 2285–2486 | Compound assignment + pointer arithmetic |
| 2487–2635 | Binary node construction with vector decomposition |
| 2636–2889 | Complex arithmetic helpers (`(a+bi)(c+di)` etc.) |
| 2890–2908 | `find_member` |
| 2909–4017 | Expression grammar (the bulk: `assign` → `conditional` → ... → `primary`) |

**Status:** skeleton.  Substantive drafting follows `04a_decl.md`.

---

## A. Precedence ladder overview

[The recursive-descent function chain, from lowest to highest
precedence:

`expr` → `assign` → `conditional` → `logor` → `logand` → `bitor` →
`bitxor` → `bitand` → `equality` → `relational` → `shift` →
`add` → `mul` → `cast` → `unary` → `postfix` → `primary`

Plus sideways links from `cast` to type-name parsing in
`04a_decl.md` §C.1, and from `primary` to compound-literal /
statement-expression / `_Generic` handlers below.]

## B. `expr` and the comma operator

[Top-level `expr` parses `assign` then a sequence of `, assign`.
Result type is the type of the last operand; effects are
sequenced.  Comma-as-argument-separator (function calls) is
distinguished by context.]

## C. `assign` and compound assignment

[`=`, `+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, `>>=`, `&=`, `|=`, `^=`.
Each compound form lowers to `lhs = lhs op rhs` with a single
evaluation of `lhs` (use a tmp if `lhs` has side effects).  Pointer
arithmetic for `+=` / `-=` (per §J).]

## D. `conditional` (`?:`)

[Three-operand ternary.  Type of result is the common type of the
two branches per `usual_arith_conv` (type-system §7).  Side
effects: only the selected branch is evaluated.  `?:` with omitted
middle (`a ?: b` GCC extension) — not in scope unless main accepts
it; check.]

## E. Logical, bitwise, equality, relational, shift, additive,
multiplicative

[Standard binary operators.  Result types per type-system §7.  All
go through `new_binary_with_decomposition` (§I) to handle vector
operands.]

## F. `cast` and parenthesized forms

[Three productions starting with `(`:
1. `(type){...}` — compound literal.  Type is the abstract
   declarator inside the parens; body is an initializer.  Lowers
   to a tmp local + initialization (see `04d_init.md` §F).
2. `(type)expr` — cast.  Wraps `expr` via `new_cast`.
3. `(expr)` — parenthesized expression.  Result is `expr`'s tree.

Disambiguation is via two-token lookahead per §5.4 of
`04_parse.md`: peek `tok->next`; if `is_typename(tok->next)`,
parse as type-name and then either compound literal (`{`) or cast
(anything else).]

## G. `unary`

[Prefix operators: `++`, `--`, unary `+`, unary `-`, `!`, `~`,
`*` (deref), `&` (address-of), `sizeof`, `_Alignof`, `__alignof__`,
`__real__`, `__imag__`, `&&label` (labels-as-values).  Each has a
specific Node kind; address-of has special rules for arrays
(decay-blocking) and functions.]

### G.1 `sizeof` and `_Alignof`

[Two forms: `sizeof expr` and `sizeof(type-name)`.  The
`is_typename` check on `(...)` content disambiguates.  Result
type is `size_t`.  For VLA operands, the size expression must be
emitted (with side effects) — VLA `sizeof` is runtime.]

### G.2 `&&label`

[Address of a label as a `void *`.  Used by `goto *expr`.  Codegen
materializes; parser produces `ND_LABEL_VAL`.]

### G.3 `__real__` / `__imag__`

[On a `_Complex T`, accesses the real or imaginary half.  Lowered
to a member access on the underlying 2-tuple struct.  See §H.]

## H. `postfix` and primary

### H.1 Function calls

[`f(a, b, c)`.  Argument type adjustment per prototype (or default
argument promotion if no prototype).  `__builtin_*` names dispatched
to per-builtin handlers (see §M).]

### H.2 Member access

[`s.m` and `p->m`.  Member lookup via `find_member`, which walks
embedded-struct chains for anonymous members.  Bit-field access
produces a special Node form.]

### H.3 Array indexing

[`a[i]` lowers to `*(a + i)`.  Pointer arithmetic per §J.]

### H.4 Postfix `++` / `--`

[`x++` evaluates `x`, increments, returns pre-increment value.
Lowered to a sequence: `(tmp = x, x = x + 1, tmp)`.]

### H.5 Compound literals

[`(type){init}` in an expression context.  Produces a tmp local of
`type`, runs the initializer, evaluates to the tmp.  See
`04d_init.md` §F for init lowering.]

### H.6 Statement expressions

[`({ stmt; stmt; expr; })` GCC extension.  Returns the value of
the final expression.  Pushes a scope for the block.  See
`04c_stmt.md` §G for interaction with statement parsing.]

### H.7 `_Generic`

[`_Generic(controlling-expr, T1: e1, T2: e2, default: e3)`.
Selection happens at parse time per type compatibility (type-
system §6).  Only the selected branch is type-checked and emitted;
others are syntactically scanned but discarded.]

## I. Vector decomposition wrapper around `new_binary`

[`new_binary_with_decomposition` (or whatever the spec calls it):
if either operand is a vector type, decompose to N element-wise
scalar binaries.  See `04_parse.md` §8.3 for the lowering shape.]

## J. Pointer arithmetic

[`ptr + int` and `int + ptr` scale the integer by the pointee
size.  `ptr - ptr` divides by pointee size, returns `ptrdiff_t`.
VLA-of-pointer decay handled here per `04_parse.md` §7.4.  Errors
on `void *` arithmetic (or accept per GCC, with byte-step — match
main).]

## K. Constant expression evaluation

### K.1 `eval_node` contract

[Reference §6.1 of `04_parse.md`.  Specifies the per-Node-kind
fold rules in detail.]

### K.2 `try_eval_node` contract

[Reference §6.1 of `04_parse.md`.  Same fold rules but non-fatal.]

### K.3 Float folding (`eval_double`)

[Internal helper that returns `double` (or `long double`) for
floating-point expressions.  Used by `eval_node` when result type
is float.  Subtleties: float→int conversion rules, long-double
precision.]

### K.4 Complex evaluator (`eval_complex`)

[Specialized walker for `_Complex T` expressions.  Returns a
2-tuple `(double, double)` (or long double pair).  Operator scope
matches scalar fold scope, applied component-wise.]

### K.5 Pointer-relative folding

[For static initializers: `&arr[N]` and `&obj + N` produce
relocation records, not raw integers.  Specified jointly with
`04d_init.md` §G.]

## L. Complex arithmetic helpers

[`build_complex_mul`, `build_complex_div` (or whatever names the
spec gives them).  Implement (a+bi)(c+di) = (ac-bd) + (ad+bc)i and
the analogous division formula.  Emit Node trees that reference a
tmp local of `_Complex T`.  See `04_parse.md` §8.2 for the
top-level shape.]

### L.1 Worked example: `_Complex double z = (1.0 + 2.0i) * (3.0 + 4.0i);`

[Trace through tmp-local creation, real/imag computation, final
substitution.]

## M. Builtin function dispatch

[A handful of `__builtin_*` names are recognized by the parser
and lowered specially:

- `__builtin_offsetof(type, member)` — folded to `ND_NUM` at parse
  time.
- `__builtin_types_compatible_p(t1, t2)` — folded to 0 or 1.
- `__builtin_constant_p(expr)` — folded based on whether
  `try_eval_node` succeeds.
- `__builtin_va_start`, `__builtin_va_arg`, `__builtin_va_copy`,
  `__builtin_va_end` — produce dedicated Node kinds; codegen
  lowers.
- `__builtin_expect(expr, val)` — value is `expr`; `val` is hint
  ignored by ncc (no branch profiling).
- `__builtin_choose_expr(cond, a, b)` — `cond` is parse-time
  constant; result is the selected branch.
- `__builtin_unreachable()` — Node kind for codegen.
- `__sync_*` and `__atomic_*` — Node kinds for codegen.

Each entry's exact disposition is enumerated.]

## N. `find_member`

[Struct-member-by-name lookup.  Walks anonymous-struct embedded
chains (GCC extension).  Returns `Member *` with offset chain so
codegen can compute final access offset.]

## O. Worked examples

### O.1 `((vec_t){1, 2, 3, 4} + (vec_t){5, 6, 7, 8})[2]`

[Trace through compound literals, vector addition decomposition,
indexing.]

### O.2 `_Static_assert(sizeof(int) == 4, "")`

[Trace through `try_eval_node`, the success path that suppresses
the diagnostic.]

### O.3 `int x = (int[]){1, 2, 3}[1];`

[Compound literal in initializer position, array-decay, indexing,
constant fold.]

### O.4 `&&label` and `goto *p`

[Labels-as-values address production and use.]

## P. Phase 5 prerequisites added by 04b

[AST invariants from this sub-chunk; append to `04_parse.md` §15
during impl review.]

## Q. Open questions

[Sub-chunk-specific questions raised during drafting.]
