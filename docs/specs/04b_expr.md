# ncc Parser — Expressions Sub-Chunk (Phase 4, 04b)

This sub-chunk specifies the **expression zone** of `src/parse.c`:
the precedence ladder, primary expressions, postfix operators,
unary operators, the cast/sizeof/alignof family, constant
expression evaluation (`eval_node` / `try_eval_node` /
`eval_double` / `eval_complex`), parse-time complex / vector
lowering, and the builtin-function dispatch table.

Cross-cutting decisions live in `04_parse.md`; this file cites
them.

**Coverage in `src/parse.c`:**

| Lines | Section |
|---|---|
| 118–163 | Node construction helpers (`new_node`, `new_unary`, `new_binary`, `new_num`, `new_long`, `new_ulong`, `new_var_node`) |
| 266–399 | Complex / vector helpers (`new_real`, `new_imag`, `vec_elem`, `vec_set_elem`, `vec_binary_op`, `vec_unary_op`) |
| 1764–1803 | `_Generic` selection |
| 1810–1818 | `expr` (top-level + comma) |
| 1820–2185 | Constant expression evaluation (`const_expr_val`, `eval`, `eval2`, `eval_rval`, `eval_double`, `eval_complex`, `try_eval_node`) |
| 2189–2283 | `assign` and compound assignment |
| 2286–2301 | `to_assign` (compound-assign lowering) |
| 2304–2431 | `new_add`, `new_sub` (pointer arithmetic + complex/vector) |
| 2434–2465 | `cond_expr` (`?:`) and Elvis |
| 2467–2495 | `logor`, `logand`, `new_vec_or_binary` |
| 2496–2615 | `bitor_`, `bitxor`, `bitand`, `equality`, `relational`, `shift` |
| 2618–2710 | `add`, `mul`, complex arith helpers |
| 2637–2677 | `new_complex_mul`, `new_complex_div` |
| 2713–2730 | `cast` and the cast-vs-compound-literal-vs-paren-expr disambiguator |
| 2738–2888 | `unary` |
| 2891–2945 | `find_member` (legacy) and `struct_ref` |
| 2949–3084 | `postfix` |
| 3086–3121 | `new_inc_dec` (post-inc/dec lowering) |
| 3128–3895 | `primary` and the builtin dispatch table |
| 3898–end-of-funcall | `funcall` |

**Status:** substantive.

When this document and `main`'s observable behavior disagree, this
document is wrong and must be updated, unless the divergence is
recorded in §13 of `04_parse.md`.

---

## A. Precedence ladder overview

The recursive-descent function chain, lowest to highest
precedence:

```
expr            ← assign ("," assign)*
assign          ← cond_expr [ assign-op assign ]
cond_expr       ← logor [ "?" expr ":" cond_expr ]
                | logor "?" ":" cond_expr           // GNU Elvis
logor           ← logand ("||" logand)*
logand          ← bitor_ ("&&" bitor_)*
bitor_          ← bitxor  ("|"  bitxor)*
bitxor          ← bitand  ("^"  bitand)*
bitand          ← equality ("&" equality)*
equality        ← relational ( ("==" | "!=") relational )*
relational      ← shift ( ("<" | "<=" | ">" | ">=") shift )*
shift           ← add (("<<" | ">>") add)*
add             ← mul ( ("+" | "-") mul )*
mul             ← cast ( ("*" | "/" | "%") cast )*
cast            ← "(" type-name ")" cast
                | "(" type-name ")" "{" init-list "}"   // compound literal
                | unary
unary           ← prefix-op cast
                | ("++" | "--") unary
                | "sizeof" unary | "sizeof" "(" type-name ")"
                | "_Alignof" "(" type-name ")"
                | "&&" ident                              // labels-as-values
                | "__real__" cast | "__imag__" cast
                | postfix
postfix         ← compound-literal-from-(T){...}
                | primary postfix-suffix
primary         ← builtin-call | identifier | constant | string | "(" expr ")"
                | "(" "{" stmt "}" ")"   // statement expression
                | "_Generic" "(" generic-list ")"
postfix-suffix  ← "[" expr "]" | "." ident | "->" ident
                | "(" arg-list ")" | "++" | "--"
```

The `assign` operators (`=`, `+=`, `-=`, ...) are right-associative
via the `assign ← cond_expr (op assign)?` recursive form.

The `cond_expr` `?:` is right-associative (condition's else is
`cond_expr`, not `expr`).

All other binary operators are left-associative via the iterative
`while (equal(tok, op))` loop pattern.

---

## B. `expr` and the comma operator

`expr(Token **rest, Token *tok)`:

1. Parse `assign` into `node`.
2. If next token is `,`, recurse: `node = new_binary(ND_COMMA,
   node, expr(rest, tok->next), tok)`.
3. Otherwise return `node`.

The recursion makes the comma operator right-associative in the
AST, but commutativity of comma's effect (each operand is
evaluated, the rightmost's value is the result) makes the
associativity invisible at runtime.

Comma in argument lists (`f(a, b, c)`) and initializer lists
(`{1, 2, 3}`) is **not** the comma operator — the parser routes
those callsites through `assign` rather than `expr`, so
comma-as-separator wins the parse.

---

## C. `assign` and compound assignment

`assign(Token **rest, Token *tok)`:

1. Parse `cond_expr` into `node`.
2. If next token is `=`, recurse on RHS via `assign`.  Apply
   complex-cast adjustment if needed (§C.1).  Return
   `new_binary(ND_ASSIGN, node, rhs, tok)`.
3. Else if next token is one of `+=`, `-=`, `*=`, `/=`, `%=`,
   `&=`, `|=`, `^=`, `<<=`, `>>=`: dispatch by operator-class
   (§C.2–C.5).
4. Else return `node`.

### C.1 Plain `=` and complex-cast injection

For `node = rhs` where `node` has complex type:
- If `rhs->ty->kind != node->ty->kind` (rhs is scalar or
  different-shape complex), insert
  `rhs = new_cast(rhs, node->ty)`.
- Else if both are complex but with different base kinds, insert
  the cast as well.

This ensures the assignment lowers to a complex-typed store; the
type system's `add_type` handles further validation.

### C.2 Bitfield compound assignment

If `node->kind == ND_MEMBER && node->member->is_bitfield`:

The standard `to_assign` lowering — `tmp = &lhs; *tmp = *tmp op
rhs` — would lose bit-field metadata behind the pointer.  Instead,
emit the bit-field-direct form: `lhs = lhs op rhs`, with `op`
selected from the operator (`+=` → `new_add`, `-=` → `new_sub`,
otherwise → `new_binary(op, ...)`).

### C.3 Complex compound assignment

If `is_complex(node->ty)`:
- `+=` → `new_add(node, rhs)`.
- `-=` → `new_sub(node, rhs)`.
- `*=` → `new_complex_mul(node, rhs)`.
- `/=` → `new_complex_div(node, rhs)`.
- Other operators (`%=`, `<<=`, `&=`, etc.) → error "invalid
  complex compound assignment".

Result wrapped in `new_binary(ND_ASSIGN, node, val)`.

### C.4 Vector compound assignment

If `node->ty->kind == TY_VECTOR`:
- `+=`, `-=` → `new_add` / `new_sub` (which handle vectors).
- Other operators → `new_vec_or_binary(op, ...)`.

Result wrapped in `new_binary(ND_ASSIGN, node, val)`.

### C.5 Scalar compound assignment via `to_assign`

For all other cases, build `binary = new_add(...)` /
`new_sub(...)` / `new_binary(op, ...)` and pass to `to_assign`.

`to_assign(binary)` lowers to:

```
tmp = &binary->lhs;
*tmp = *tmp op binary->rhs
```

where `tmp` is a fresh anonymous local of `pointer_to(binary->
lhs->ty)`.  The result is `(tmp = &lhs, *tmp = *tmp op rhs)`
expressed as a chain of `ND_COMMA` nodes.  The address-of
indirection ensures `lhs` is evaluated only once even with
side-effecting subexpressions like `a[f()] += 1`.

---

## D. `cond_expr` (ternary `?:`)

`cond_expr(Token **rest, Token *tok)`:

1. Parse `logor` into `node`.
2. If next token is not `?`, return `node`.
3. **GNU Elvis form** `a ?: b`: if next-next token is `:`, lower
   to a tmp-var pattern that evaluates `node` once:

   ```
   (tmp = node, tmp ? tmp : b)
   ```

   Built as:
   ```
   ND_COMMA
   ├─ ND_ASSIGN(tmp_var1, node)
   └─ ND_COND
      ├─ cond: tmp_var2
      ├─ then: tmp_var (third reference)
      └─ els:  cond_expr(rest, ...)
   ```

   The else branch is parsed via `cond_expr` (not `expr`), so
   `?:` chains right-associatively.

4. **Standard form** `a ? b : c`: build `ND_COND` with `cond =
   node`, `then = expr(...)`, `els = cond_expr(rest, ...)`.

   Note `then` is parsed as `expr` (allows comma operator
   inside), but `els` is `cond_expr` (so chained `?:` recurses
   into the right associatively).

---

## E. Logical, bitwise, equality, relational, shift, additive, multiplicative

Standard left-to-right binary operators, each implemented as
`while (equal(tok, op)) node = new_binary(...)`.

### E.1 `||` and `&&`

`logor` and `logand`: build `ND_LOGOR` / `ND_LOGAND` with `new_
binary` directly (no vector decomposition — logical operators
on vectors aren't accepted).

### E.2 Bitwise `|`, `^`, `&`

`bitor_`, `bitxor`, `bitand`: route through `new_vec_or_binary`
so vector operands element-wise-decompose.

### E.3 Equality `==`, `!=`

Handled inline (not via `new_vec_or_binary`) because complex
operands need a special lowering:

- Vector operand: `vec_binary_op(ND_EQ, ...)` / `vec_binary_op
  (ND_NE, ...)`.
- Complex operand: lower to `(re_l == re_r) && (im_l == im_r)`
  for `==`; `(re_l != re_r) || (im_l != im_r)` for `!=`.
  Scalar operand is treated as `(scalar, 0)` — i.e., real part
  is the scalar, imag is zero.
- Otherwise: `new_binary(ND_EQ/ND_NE, ...)`.

### E.4 Relational `<`, `<=`, `>`, `>=`

`new_vec_or_binary` for vectors; otherwise `new_binary(ND_LT)`
/ `new_binary(ND_LE)`.  `>` and `>=` are emitted as
`ND_LT` / `ND_LE` with operands swapped — there are no `ND_GT`
or `ND_GE` Node kinds.

### E.5 Shift `<<`, `>>`

`new_vec_or_binary(ND_SHL/ND_SHR, ...)`.

### E.6 Additive `+`, `-`

`add(...)`: dispatch through `new_add` / `new_sub`, which handle
pointer arithmetic, complex/scalar mixing, and vector
decomposition.  See §J.

### E.7 Multiplicative `*`, `/`, `%`

`mul(...)`:
- `*` and `/` with complex operand → `new_complex_mul` /
  `new_complex_div` (§L).
- `*` and `/` otherwise → `new_binary(ND_MUL/ND_DIV)`.
- `%` → `new_vec_or_binary(ND_MOD)` (no complex; complex
  modulo is undefined in C and rejected by type system).

---

## F. `cast` and the parenthesized-form disambiguator

`cast(Token **rest, Token *tok)`:

1. If next is not `(`, fall through to `unary`.
2. If `is_typename(tok->next)` is **false** (next-token is not a
   type-name), fall through to `unary` — this is a parenthesized
   expression handled by `primary`.
3. Otherwise, the `( typename` opener is committed.  Parse
   `typename_` to obtain `Type *ty`.
4. Skip the closing `)`.
5. **Compound literal vs cast disambiguator**: if next token is
   `{` and `ty->kind != TY_VOID`, this is a compound literal.
   Reset position and re-enter `unary` so `postfix` can handle
   the compound literal (which has its own logic at the top of
   `postfix`).
6. Otherwise (cast): recursively parse `cast` for the operand,
   wrap in `new_cast(operand, ty)`.

This is the only two-token-lookahead path in the parser per
`04_parse.md` §5.4: when seeing `(`, peek `tok->next` and call
`is_typename(tok->next)` to commit to the cast/literal branch.

`(void)` cast is permitted (the operand's value is discarded).
`(void){...}` compound literal is rejected (void compound
literal is meaningless).

---

## G. `unary`

Each prefix operator handler returns immediately after
constructing the node; there is no "fall through".

### G.1 `+x`

Returns `cast(rest, tok->next)` directly.  Unary plus has no
runtime effect (per C11 §6.5.3.3) — the parser drops the
operator entirely.

### G.2 `-x`

- Vector operand: `vec_unary_op(ND_NEG, ...)`.
- Complex operand: `new_complex_val(-re, -im, cty, tok)` —
  negate both halves.
- Otherwise: `new_unary(ND_NEG, ...)`.

### G.3 `&x` (address-of)

`new_unary(ND_ADDR, cast(...), tok)`.  No special-case for
arrays or functions — the type system's `add_type` handles
"address-of array gives pointer-to-element" and "address-of
function gives pointer-to-function" semantics.

### G.4 `*x` (indirection)

`new_unary(ND_DEREF, cast(...), tok)` followed by `add_type` so
the resulting type is set immediately (some downstream
inspection paths need it).

### G.5 `!x` (logical not)

`new_unary(ND_NOT, cast(...), tok)`.

### G.6 `~x` (bitwise not)

- Vector operand: `vec_unary_op(ND_BITNOT, ...)`.
- Complex operand: complex conjugate — `new_complex_val(re,
  -im, ...)` (negates imaginary part only).  This is GCC's
  semantics for `~` on complex types.
- Otherwise: `new_unary(ND_BITNOT, ...)`.

### G.7 `__real__ x`, `__imag__ x`

`new_unary(ND_REAL/ND_IMAG, cast(...), tok)`, with `add_type`
called immediately so the result type is the underlying
real-typed half.

### G.8 `++x`, `--x` (pre-inc/dec)

For non-bitfield: `to_assign(new_add(operand, new_num(±1, tok),
tok))` — the standard compound-assign lowering pattern (§C.5).

For bitfield (`operand->kind == ND_MEMBER && operand->member->
is_bitfield`): direct `lhs = lhs ± 1` form, no `to_assign` wrap.

The recursion is `unary(rest, tok->next)` — `++x++` is `++(x++)`,
right-associative.

### G.9 `&&label` (labels-as-values)

```
&& IDENT
```

Builds `ND_LABEL_VAL` with `label = strndup_checked(...)` of the
identifier text.

To preserve uniqueness across multiple `&&label` references to
the same label name (which can happen when an initializer is
re-parsed for a flexible array member), the parser walks the
existing `gotos` chain looking for prior `ND_LABEL_VAL` with the
same `label` and reuses its `unique_label`.

The unique_label format is `_cg_%d` (not the typical `.L%d`):
labels on macOS prefixed with `L` are stripped as local symbols,
so a non-`L` prefix is required for cross-section references
(e.g., labels-as-values stored in a `.data` table).

The new node is prepended to `gotos` for end-of-function
resolution (`04c_stmt.md` §E.1).

### G.10 `sizeof`

Three forms:
- `sizeof ( type-name )`: parse `typename_`, return:
  - `new_var_node(ty->vla_size, tok)` if `ty->vla_size` is set
    (VLA or struct-with-VLA member).
  - `new_ulong(ty->size, tok)` otherwise.
- `sizeof unary-expr`: parse `unary`, run `add_type`, then:
  - If `node->kind == ND_VAR && node->var->ty->vla_size`, use
    the variable's original type (which still has `vla_size`).
  - If `node->ty->vla_size`, use that.
  - Otherwise: `new_ulong(node->ty->size, tok)`.

The "check the variable's original type before decay" branch
matters for `sizeof a` where `a` is a VLA parameter — the
parameter type has decayed to pointer, but the original VLA
size variable is still attached via `var->ty->vla_size`.

`sizeof` of an incomplete type produces `node->ty->size = -1`,
which is an error caller-side (the type system rejects it).

### G.11 `_Alignof` / `__alignof__` / `__alignof`

```
_Alignof ( type-name )           // type form
_Alignof unary-expr              // expression form (ncc accepts)
```

Form is detected by `is_typename(tok)` after the `(`.  Returns
`new_ulong(ty->align)` for the type form.

For the expression form: parse `unary`, run `add_type`.  The
result is `node->ty->align`, but if `node->kind == ND_VAR &&
node->var->align > node->ty->align`, use the variable's explicit
alignment (`__attribute__((aligned(N)))` overrides).

C11 §6.5.3.4 specifies only the type form; the expression form
is a GCC extension ncc accepts.  The closing `)` is required for
both — the expression form parses through the parenthesis as
part of the operator syntax, not as a parenthesized expression.

---

## H. `postfix` and primary

### H.1 Compound literal at the head

`postfix` first checks for `( type-name )` followed by `{`.  If
so:
- Parse `typename_`.
- Skip `)`.
- If next token is `{`:
  - **In a function** (`scope->next != NULL`): create a fresh
    local via `new_lvar("", ty)`, parse `lvar_initializer` into
    the new var, build `ND_COMMA(init_block, var_ref)` so the
    expression evaluates the initialization and yields the var.
  - **At file scope**: create `new_anon_gvar(ty)`, parse
    `gvar_initializer` (which writes to `init_data` /
    relocations), the expression yields a reference to the
    anonymous global.
  - In both cases, fall through to the postfix-suffix loop so
    `(T){...}[i]`, `(T){...}.field`, `(T){...}->field`, etc.
    parse correctly.
- If next token is not `{`, the `( type-name )` was a cast
  after all — re-enter `cast` from the start position.

### H.2 Postfix suffix loop

Loops over zero or more suffixes:

**`[ expr ]`** (subscript):
- Vector operand: cast `&node` to `pointer_to(elem_type)`,
  add the index, dereference.  Result type is element type.
- Otherwise: `*(node + idx)` via `new_add` then `ND_DEREF`.

**`. ident`** and **`-> ident`**:
- For `->`, first wrap `node` in `ND_DEREF`.
- Call `struct_ref(node, ident_tok)` (§N) to walk the member
  chain (handles anonymous struct members).  Error "no such
  member" if `struct_ref` returns NULL.

**`( arg-list )`** (function call through expression):
- `add_type(node)` to get the function type:
  - If `node->ty->kind == TY_FUNC`, use directly.
  - Else if `node->ty->kind == TY_PTR && node->ty->base->kind
    == TY_FUNC`, use `node->ty->base`.
  - Otherwise, this is not a callable — leave the loop.
- Parse comma-separated argument expressions via `assign`.
  For each:
  - If the next parameter type is non-aggregate
    (`!= TY_STRUCT && != TY_UNION && != TY_VECTOR`), insert
    `new_cast(arg, param_ty)` to coerce.
  - If beyond the named-parameter list (variadic) and `arg->ty
    == TY_FLOAT`, promote to `ty_double` (default argument
    promotion per C11 §6.5.2.2/7).
- Build `ND_FUNCALL` with `func_ty`, `ty = func_ty->return_ty`,
  `args = arg-list`, `lhs = node` (the callable expression),
  and `funcname = node->var->name` if directly named, else
  `"__indirect_call"`.

**`++` / `--`** (post-inc/dec): call `new_inc_dec(node, tok,
±1)` (§H.3).

When no more suffixes match, return `node`.

### H.3 `new_inc_dec` (post-inc/dec lowering)

For non-bitfield: build the side-effecting comma chain
```
(tmp = &x, old = *tmp, *tmp = *tmp + addend, old)
```
where `tmp` is a fresh `pointer_to(node->ty)` local and `old` is
a fresh `node->ty` local.

For bitfield:
```
(old = bf, bf = bf + addend, old)
```
direct form, no pointer indirection.

Both paths return `old` as the comma-chain's value, matching C
semantics for `x++`.

### H.4 `primary` dispatch order

`primary` checks token spelling in the following order (each
branch returns immediately on match):

1. Builtin functions (§M, ~25 distinct builtins).
2. `( { stmt }` — GNU statement expression: `ND_STMT_EXPR` with
   body from `compound_stmt`.
3. `( expr )` — parenthesized expression.
4. `_Generic` — call `generic_selection`.
5. String literal (`TK_STR`), with adjacent-literal concatenation
   (`"foo" "bar"` → single literal of length 7 incl. NUL).
6. Numeric literal (`TK_NUM`):
   - If `is_complex(tok->ty)` (imaginary literal like `1.0i`):
     build `new_complex_val(zero, imag, ty)`.
   - If `is_flonum(tok->ty)`: `ND_NUM` with `fval` set.
   - Otherwise: `ND_NUM` with `val` set.
7. `__func__` / `__FUNCTION__` / `__PRETTY_FUNCTION__`: emit a
   string literal containing `current_fn->name`.
8. Identifier:
   - Look up via `find_var`.
   - If next token is `(` and the variable is a function pointer
     (`TY_PTR` with `TY_FUNC` base), produce `ND_VAR` and let
     `postfix` handle the call.
   - If next is `(` and not a function pointer: dispatch to
     `funcall` (§H.5) which handles implicit declaration.
   - Otherwise, look up as variable (`new_var_node(vs->var)`)
     or enum constant (`new_num(vs->enum_val)`).
   - Undefined identifier → error.
9. Fall-through error: "expected an expression".

### H.5 `funcall` (direct call by identifier)

Used when `primary` sees `IDENT (`.  Differs from
postfix's call branch by handling implicit function declarations
(undeclared functions).

1. Look up the function in scope.  If found and `TY_FUNC`, use
   its type; if found as `TY_PTR → TY_FUNC`, use the base.
2. If not declared: synthesize a `func_type(ty_int)`, treat as
   variadic if the name is one of the known variadic libc
   functions (`printf`, `fprintf`, `sprintf`, `snprintf`,
   `scanf`, `sscanf`).  Each of these gets synthesized named
   parameters of the right shape (`const char *` for the format
   string, plus prefix args for variants like `snprintf`).
3. Parse argument list, applying parameter-driven casts and
   default argument promotion as in §H.2.
4. **Nested-function chain pointer**: if the callee is a nested
   function (`vs->var->is_nested`), append a chain pointer as
   the final argument.  The chain's value is:
   - The current frame address (`ND_FRAME_ADDR`) if the callee
     is a direct child of the current function.
   - The current function's chain parameter (`current_fn->
     chain_param`) if the callee is a sibling or higher.
   - Otherwise the current frame address (defensive).
5. Build `ND_FUNCALL` with `funcname = vs->var->name`, `func_ty`,
   `ty = return type`, `args = arg list`.

---

## I. `new_vec_or_binary` (vector-decomposing binary)

Helper used by most binary operators (`bitor_`, `bitxor`,
`bitand`, `relational`, `shift`, `mul%`, `add`):

```c
add_type(lhs); add_type(rhs);
if (lhs->ty->kind == TY_VECTOR || rhs->ty->kind == TY_VECTOR)
  return vec_binary_op(kind, lhs, rhs, tok);
return new_binary(kind, lhs, rhs, tok);
```

`vec_binary_op` (parse.c §164–399) decomposes:
1. Allocate a tmp local of vector type.
2. For each element index `i` in `0..N-1`:
   - Extract `lhs[i]` via `vec_elem` (loads element `i`).
   - Extract `rhs[i]`.
   - Compute scalar binary `op` on the two scalars.
   - Store into `tmp[i]` via `vec_set_elem`.
3. Final value of the expression is `tmp`.

The emitted form is a comma-chain of N stores plus a final
reference to `tmp`.  See `04_parse.md` §8.3 for the lowering
contract.

`vec_elem(vec, i, tok)` produces:
```
*((elem_t *)&vec + i)
```
i.e., it casts the vector's address to a pointer-to-element and
indexes.  This works because vectors are laid out contiguously
as an array of their base type.

`vec_set_elem(vec_var, i, val, tok)` produces:
```
*((elem_t *)&vec_var + i) = val
```

---

## J. Pointer arithmetic (`new_add`, `new_sub`)

### J.1 `new_add`

1. `add_type` both operands.
2. **Function-to-pointer decay**: if either operand is `TY_FUNC`,
   wrap in `pointer_to(...)` (per C11 §6.3.2.1).  This decay
   happens here, not in the type system, because `add_type` does
   not auto-decay function values (only arrays).
3. **Vector**: if either operand is a vector, return
   `vec_binary_op(ND_ADD, lhs, rhs, tok)`.
4. **Complex**: if either operand is complex, lower to
   element-wise add on real and imaginary halves.  Common base:
   the wider of the two complex bases (or `ty_double` /
   `ty_ldouble` if either is `double` / `long double`).  Scalar
   operand contributes `(scalar, 0)`.
5. **num + num**: `new_binary(ND_ADD, lhs, rhs)`.
6. **ptr + ptr**: error "invalid operands".
7. **Canonicalize**: if `num + ptr`, swap to `ptr + num`.
8. **ptr + num**: scale `num` by element size.  Element size is:
   - `lhs->ty->base->vla_size` (a Node referencing the runtime
     size variable) if the base is a VLA / struct-with-VLA.
   - `lhs->ty->base->size` otherwise (or `1` if the base is
     incomplete or function — GCC byte-arithmetic for these).
   Build `new_binary(ND_MUL, num, elem_size)`, then
   `new_binary(ND_ADD, ptr, scaled_num)`.

### J.2 `new_sub`

1. `add_type` both operands.
2. Function-to-pointer decay (same as `new_add`).
3. Vector: `vec_binary_op(ND_SUB, ...)`.
4. Complex: element-wise subtract.  Note: the common-type
   selection here is simpler than `new_add` — uses whichever
   operand is complex without widening.  Discrepancy worth
   tracking; see §Q.
5. num - num: `new_binary(ND_SUB, ...)`.
6. ptr - num: scale `num` by element size, subtract.  Result
   type explicitly set to `lhs->ty` (preserving pointer-typed
   result).
7. ptr - ptr: subtract pointer values, then divide by element
   size.  Result type `ty_long` (per C11 §6.5.6/9 — `ptrdiff_t`,
   which on this target is `long`).  Same VLA/incomplete
   special-cases as `new_add`.
8. Other combinations → error "invalid operands".

---

## K. Constant expression evaluation

There are six entry points, each with a distinct contract:

| Function | Returns | Contract |
|---|---|---|
| `eval(node)` | `int64_t` | Internal helper; calls `eval2` with NULL `label`. |
| `eval_node(node)` | `int64_t` | Public C-callable per `04_parse.md` §2; forwards to `eval2`. |
| `eval2(node, label)` | `int64_t` | The full integer fold; `label` is an optional out-pointer for static-address relocations. |
| `try_eval_node(node, out)` | `bool` | Non-fatal; fills `*out` and returns `true` if foldable. |
| `eval_double(node)` | `double` | Float-typed fold. |
| `eval_complex(node, re, im)` | (out via pointers) | Complex fold; works on the lowered form. |
| `eval_rval(node, label)` | `int64_t` | Internal helper for address-of fold (used in initializers and offsetof patterns). |

`const_expr_val(rest, tok)` is a parsing wrapper: parse a
`cond_expr` and call `eval` on the result.

### K.1 `try_eval_node` fold scope

Per `04_parse.md` §6, the non-fatal evaluator handles the same
operator set as `eval2` but bails (returns `false`) on anything
not in its switch.  The supported kinds are:

`ND_NUM`, `ND_NEG`, `ND_NOT`, `ND_BITNOT`, `ND_CAST`, `ND_ADD`,
`ND_SUB`, `ND_MUL`, `ND_DIV`, `ND_MOD`, `ND_BITAND`, `ND_BITOR`,
`ND_BITXOR`, `ND_SHL`, `ND_SHR`, `ND_EQ`, `ND_NE`, `ND_LT`,
`ND_LE`, `ND_LOGAND`, `ND_LOGOR`, `ND_COND`.

Notably **not** in `try_eval_node`:
- `ND_VAR` — even for static-address objects.
- `ND_ADDR`, `ND_DEREF`, `ND_MEMBER` — no offsetof support.
- `ND_REAL`, `ND_IMAG`.
- Builtin folds (`__builtin_clz` etc.).
- Cast through float.

`try_eval_node` is for C11's `_Static_assert` integer-constant
case — full constant folding goes through `eval2`.

`ND_DIV` and `ND_MOD` short-circuit on zero divisor (return
`false`).  This is the conservative behavior: rather than
producing UB, declare non-constant.

### K.2 `eval2` fold scope

The full evaluator handles everything `try_eval_node` does plus:

**Float dispatch:** if `is_flonum(node->ty)`, return
`(int64_t)eval_double(node)`.  Float-to-int conversion via C
cast.

**Pointer-relative folding (`label` non-NULL):**
- `ND_ADDR` of an `ND_VAR` (non-local): writes
  `*label = &node->var->name` and returns 0.  Caller emits a
  relocation `(symbol, offset=0)`.
- `ND_VAR` (non-local, non-array, non-function) with name
  starting `.L.data.` and `init_data` set: anonymous compound
  literal at file scope.  Resolve via the variable's own
  relocation list (§K.4).  Otherwise: error.
- `ND_VAR` (any other static): writes `*label = &node->var->
  name`, returns 0.
- `ND_MEMBER`: recurses on `lhs` via `eval_rval` and adds the
  member offset.  In offsetof-like patterns where `lhs` resolves
  to a pure numeric base (`(struct T *)0`), no label is
  produced.
- `ND_LABEL_VAL`: writes `*label = &node->unique_label`,
  returns 0.

**Equality with relocation:**
`ND_EQ` and `ND_NE` against a static-address operand:
- If both operands have NULL labels, plain integer compare.
- If one has a label and the other is the literal 0, `==`
  produces 0 (a static address is never null) and `!=` produces
  1.
- If both have labels and the labels point to the same symbol
  pointer, plain integer compare on the offsets.
- Otherwise error "not a compile-time constant".

This branch enables the kernel's `_OF_DECLARE` pattern: `(fn ==
(fn_t)NULL) ? fn : fn` is a typecheck idiom that must fold to
the function pointer.

**Conditional with relocation:**
`ND_COND` evaluates its condition through `eval2` with a fresh
`label` slot: if the result has a non-NULL `label`, the value is
treated as truthy regardless of the integer (a non-null pointer
is always truthy).

**Cast truncation:**
`ND_CAST` to a sized integer type performs the appropriate
bit-width truncation: `(int8_t)(...)` for 1-byte, etc.

**Builtin folds:**
`ND_BUILTIN_CLZ`, `ND_BUILTIN_CTZ`, `ND_BUILTIN_FFS`,
`ND_BUILTIN_POPCOUNT`, `ND_BUILTIN_PARITY`, `ND_BUILTIN_CLRSB`,
`ND_BUILTIN_BSWAP32`, `ND_BUILTIN_BSWAP64` — fold using the
host's matching `__builtin_*` (this is one of the few places ncc
relies on the host compiler's intrinsics).

`__builtin_clz/ctz/ffs/clrsb` use the `node->val` field as a
flag for 32-bit vs 64-bit dispatch (set by `primary` per the
suffix `l`/`ll`).

### K.3 `eval_double`

Float fold.  If the operand has integer type, fold via `eval`
and convert (signed or unsigned per `ty->is_unsigned`).

For complex operands: extract real part via `eval_complex`.

For `ND_NUM`: return `node->fval`.

For binary ops (`ADD`, `SUB`, `MUL`, `DIV`, `NEG`, `COND`,
`COMMA`, `CAST`): fold operands, apply.

`ND_REAL` and `ND_IMAG` on complex operands extract the
appropriate half.

Anything else → error "not a constant expression".

### K.4 `eval_complex`

The complex evaluator is keyed on the **lowered form** that
parse.c emits for complex literals:

```
ND_COMMA
├─ ND_COMMA
│  ├─ ND_ASSIGN(ND_REAL(tmp), R)
│  └─ ND_ASSIGN(ND_IMAG(tmp), I)
└─ ND_VAR(tmp)
```

`eval_complex` recognizes this shape, extracts `R` and `I` from
the inner assigns, and folds them via `eval_double`.

Also handles:
- `ND_CAST` from scalar to complex: `re = eval_double(lhs); im
  = 0`.
- `ND_CAST` between complex types: recurse.
- `ND_NUM` (a real number cast to complex contextually): `re =
  fval; im = 0`.

Anything else → error.

### K.5 `eval_rval`

Used when a non-NULL `label` slot needs to capture an address:

| Node kind | Action |
|---|---|
| `ND_VAR` (local) | error "not a compile-time constant" |
| `ND_VAR` (non-local) | `*label = &node->var->name`; return 0 |
| `ND_DEREF` | recurse on `lhs` via `eval2` |
| `ND_MEMBER` | recurse on `lhs` plus offset |
| `ND_CAST` | recurse on `lhs` |
| `ND_NUM` | return `node->val` (offsetof base) |
| else | error |

This service is consumed primarily by `04d_init.md` §G (global
initializer evaluation).

### K.6 Anonymous-compound-literal-as-pointer special case

Inside `eval2`'s `ND_VAR` case, an extra branch detects:
```c
(struct T *){ &(struct T){ ... } }
```

Where the inner compound literal becomes an anonymous file-scope
global with `init_data` populated.  The outer compound literal
is also an anonymous global (recognizable by name prefix
`.L.data.`) with `kind == TY_PTR` and one relocation pointing at
the inner.

When `eval2` is asked for the outer's value (with `label` slot),
it digs into the relocation list:
- If exactly one relocation at offset 0: use its `(label,
  addend)` directly.
- If no relocations and `init_data_size >= 8`: read 8 bytes as
  the pointer value (e.g., `(T*){NULL}` or `(T*){(T*)0x1000}`).

This pattern appears in Linux kernel sources (`STM32_GATE` in
`clk-stm32mp1.c`, etc.) — initializers that use compound
literals as immediate values.

---

## L. Complex arithmetic helpers

`new_complex_mul` and `new_complex_div` build `ND_COMMA` chains
that produce a fresh `_Complex T` tmp local with the right real
and imaginary halves.

### L.1 `new_complex_mul`

(a + bi) × (c + di) = (ac − bd) + (ad + bc)i.

```c
Type *cty = is_complex(lhs->ty) ? lhs->ty : rhs->ty;
Node *a = is_complex(lhs->ty) ? new_real(lhs) : lhs;
Node *b = is_complex(lhs->ty) ? new_imag(lhs) : new_num(0);
Node *c = is_complex(rhs->ty) ? new_real(rhs) : rhs;
Node *d = is_complex(rhs->ty) ? new_imag(rhs) : new_num(0);
Node *real = (a*c) - (b*d);
Node *imag = (a*d) + (b*c);
return new_complex_val(real, imag, cty, tok);
```

`new_complex_val` (parse.c line 423) builds the lowered form
`(tmp.__real__ = R, tmp.__imag__ = I, tmp)`.

### L.2 `new_complex_div`

(a + bi) / (c + di) = ((ac + bd) + (bc − ad)i) / (c² + d²).

```c
Type *cty = is_complex(lhs->ty) ? lhs->ty : rhs->ty;
Node *a, b, c, d as above.
Node *denom = c*c + d*d;
Node *real = (a*c + b*d) / denom;
Node *imag = (b*c - a*d) / denom;
return new_complex_val(real, imag, cty, tok);
```

Note `denom` is computed twice (once per half); a future
optimization could lift it to a tmp.  Current behavior: trust
codegen-side CSE, or leave as is (this is parser, not optimizer).

### L.3 Integer-typed complex

Per the canonical-type table in `04a_decl.md` §B.2, ncc supports
`_Complex int`, `_Complex char`, etc.  The helpers above work on
the underlying integer base, not double — `new_real` /
`new_imag` produce a node with the correct base type, and
`ND_MUL` / `ND_ADD` / `ND_SUB` / `ND_DIV` operate on that type.

### L.4 Worked example: `_Complex double z = (1.0 + 2.0i) * (3.0 + 4.0i);`

Result components:
- a = 1.0, b = 2.0, c = 3.0, d = 4.0.
- real = 1·3 − 2·4 = 3 − 8 = −5.
- imag = 1·4 + 2·3 = 4 + 6 = 10.

Emitted form (schematic):
```
ND_COMMA
├─ ND_COMMA
│  ├─ ND_ASSIGN(ND_REAL(tmp), -5.0)
│  └─ ND_ASSIGN(ND_IMAG(tmp),  10.0)
└─ ND_VAR(tmp)
```

`eval_complex` on this tree returns `re = -5, im = 10`.

---

## M. Builtin function dispatch

`primary` recognizes ~25 distinct builtins by exact identifier
match (no namespace lookup).  Each builds a specific Node form,
or folds at parse time, or both.

| Identifier | Lowering | Fold? |
|---|---|---|
| `__builtin_choose_expr(c,a,b)` | parses `c` as constant; returns AST of `a` or `b`; the un-chosen branch is brace-skipped without parse | parse-time |
| `__builtin_types_compatible_p(t1,t2)` | folds to 0 or 1 per the GCC compatibility rules (§M.1) | parse-time |
| `__builtin_va_arg(ap, T)` | comma chain: `tmp = (T*)ap; ap = (void*)((char*)ap + align(sizeof(T),8)); *tmp` | — |
| `__builtin_va_start(ap, last)` | `ap = current_fn->va_area`; `last` parsed and discarded | — |
| `__builtin_va_end(ap)` | no-op (`new_num(0)`) | — |
| `__builtin_va_copy(d,s)` | `d = s` | — |
| `__builtin_prefetch(addr, ...)` | `(addr, 0)` — evaluate first arg for side effects | — |
| `__builtin_alloca(size)` / `alloca(size)` | `ND_BUILTIN_ALLOCA` with `ty = void *` | — |
| `__builtin_clz/clzl/clzll` | `ND_BUILTIN_CLZ` with `val=is64` flag | parse-time via `eval2` |
| `__builtin_ctz/ctzl/ctzll` | `ND_BUILTIN_CTZ` likewise | parse-time |
| `__builtin_ffs/ffsl/ffsll` | `ND_BUILTIN_FFS` likewise | parse-time |
| `__builtin_popcount/popcountl/popcountll` | `ND_BUILTIN_POPCOUNT` (uniform 64-bit fold) | parse-time |
| `__builtin_parity/parityl/parityll` | `ND_BUILTIN_PARITY` | parse-time |
| `__builtin_clrsb/clrsbl/clrsbll` | `ND_BUILTIN_CLRSB` | parse-time |
| `__builtin_bswap16` | `ND_BUILTIN_BSWAP32` with `val=16` flag | (codegen-side) |
| `__builtin_bswap32` | `ND_BUILTIN_BSWAP32` | parse-time |
| `__builtin_bswap64` | `ND_BUILTIN_BSWAP64` | parse-time |
| `abs/labs/llabs` | `(tmp = arg, tmp < 0 ? -tmp : tmp)` | — |
| `__builtin_*_overflow` family | `ND_BUILTIN_ADD/SUB/MUL_OVERFLOW` Node kind | — |
| `__builtin_*_overflow_p` family | parse-time fold (returns 1 if overflow) | parse-time |
| `__builtin_constant_p(x)` | folds 1 if `try_eval_node(x)` succeeds, else 0 | parse-time |
| `__builtin_setjmp(buf)` | `ND_BUILTIN_SETJMP` Node | — |
| `__builtin_longjmp(buf, val)` | `ND_BUILTIN_LONGJMP` Node | — |
| `__builtin_conjf/conj/conjl` | complex conjugate (negate imag) | — |
| `__builtin_creal/crealf/creall` | `__real__ z` | — |
| `__builtin_cimag/cimagf/cimagl` | `__imag__ z` | — |
| `__builtin_return_address(N)` | `ND_BUILTIN_RETURN_ADDR` Node | — |
| `__builtin_frame_address(N)` | `ND_BUILTIN_FRAME_ADDR` Node | — |
| `__builtin_classify_type(x)` | parse-time fold to GCC's classification integer | parse-time |
| `__builtin_offsetof(type, member)` | folded via the offsetof pattern; returns `new_num(...)` | parse-time |

(`__builtin_offsetof` is dispatched via the same path as
hand-written `&((T*)0)->member`; ncc's preprocessor's
`offsetof` macro expands to the latter form.)

### M.1 `__builtin_types_compatible_p`

Compares two types per a hand-rolled compatibility check that
mirrors GCC's `tree_types_compatible_p`:

- Different `kind` → 0.
- Both `TY_ENUM`: compatible iff same origin (follow `origin`
  chain to roots and compare pointer identity).
- Both `TY_PTR`: pointed-to types must match in `kind`,
  `is_unsigned`, qualifiers, and recursively for pointer-to-
  pointer cases.
- Both `TY_ARRAY`: element types match (treating enum as int);
  lengths match unless one is `[]`.
- Both `TY_STRUCT` / `TY_UNION`: same `size` AND same `members`
  pointer (i.e., same defining declaration).
- Both `TY_FUNC`: matching return type and parameter list (by
  kind, size, signedness).
- Otherwise (scalar): same `is_unsigned` and same `size`.

Top-level qualifiers (the outer `const`/`volatile`) are ignored;
qualifiers on pointed-to types are honored.

### M.2 `__builtin_constant_p`

Lowered to `try_eval_node`'s success: `new_num(try_eval_node
(arg, &val) ? 1 : 0)`.

### M.3 The `*_overflow` family

`__builtin_add_overflow(a, b, res)` and the typed variants
(`saddl_overflow` etc.) all produce `ND_BUILTIN_ADD_OVERFLOW` /
`ND_BUILTIN_SUB_OVERFLOW` / `ND_BUILTIN_MUL_OVERFLOW`, with `lhs
= a`, `rhs = b`, and the result-pointer tracked separately.  The
`*_overflow_p` predicates fold at parse time.

The op is determined by substring search (`"add"`, `"sub"`,
`"mul"`) on the identifier name.

### M.4 `__builtin_setjmp` / `__builtin_longjmp`

Lightweight setjmp variants, distinct from POSIX `setjmp`.  Take
a `jmp_buf`-shaped buffer (an array of `void *` of size 5 on
ncc's target).  Codegen lowers to direct stack manipulation.

These are not Phase 4 fold targets — they produce side effects
no folder can model.

---

## N. `find_member` and `struct_ref`

### N.1 `get_struct_member` (legacy linear search)

The legacy `get_struct_member` function (parse.c line 2892,
marked `__attribute__((unused))`) walks the member list,
returning the first match by name.  Recurses into anonymous
struct/union members.

Currently unused; preserved for historical reasons.  The
canonical lookup is via `struct_ref` (§N.2).

### N.2 `struct_ref`

Builds a chain of `ND_MEMBER` nodes for accessing a member
through nested anonymous structs.

1. `add_type(node)` to get the operand's type.
2. Walk the `origin` chain to find a completed type with
   members (forward-declared structs may have `members = NULL`
   but a defined `origin`).
3. If not a struct or union, return NULL.
4. **Direct match**: walk members; first name match becomes
   `ND_MEMBER(node, member)`.  Return.
5. **Anonymous recursive**: walk again; for each anonymous
   `mem` with struct/union type, build `ND_MEMBER(node, mem)`
   and recurse `struct_ref` on it.  First successful recursion
   wins.

Not finding the member returns NULL; the caller (`postfix`'s
`.` and `->` cases) emits "no such member".

This recursion is what makes anonymous struct members behave
like direct outer-struct members:
```c
struct S {
  int a;
  struct { int x, y; };  // anonymous
};
struct S s;
s.x;   // → ND_MEMBER(ND_MEMBER(s, anonymous), x)
```

The accumulated offset (sum of each `ND_MEMBER`'s
`member->offset`) gives codegen the byte offset to the deeply
nested member.

---

## O. Worked examples

### O.1 `(int []){1, 2, 3, 4}[2]`

1. `cast` sees `(`; peeks `is_typename(int)` → true.
2. `typename_` parses `int []` → `array_of(ty_int, -1)`
   (incomplete; size set during initialization).
3. `cast` skips `)`; sees `{`, falls back to `unary` so
   `postfix` can handle.
4. `postfix` re-enters: `is_typename(int)` → true, parses
   `typename_` again, sees `{`, takes the compound-literal path.
5. Inside a function: `new_lvar("", int[4])` (size from init
   length), `lvar_initializer` parses `{1,2,3,4}`, builds an
   `ND_COMMA(init_block, var_ref)`.
6. Postfix-suffix loop: `[2]` →
   `ND_DEREF(ND_ADD(var_ref, 2))`.
7. Result: `ND_DEREF(ND_ADD(ND_VAR(tmp), 2))` with type
   `ty_int`, value 3.

### O.2 `_Static_assert(sizeof(int) == 4, "")`

1. `parse` (`04a_decl.md` §I.2) sees `_Static_assert`, calls
   `const_expr_val`.
2. `const_expr_val` parses `cond_expr` for `sizeof(int) == 4`:
   - `unary` sees `sizeof`, then `(int)`: parses `typename_` →
     `ty_int`, returns `new_ulong(4)`.
   - `equality` sees `== 4`: `new_binary(ND_EQ, ulong(4),
     int(4))`.
3. `eval` on `ND_EQ`: both folds to `4`, `eq = true`, returns
   `1`.
4. Non-zero → assertion succeeds, no diagnostic.

### O.3 `__builtin_offsetof(struct S, b)` where `S = { int a; int b; }`

The preprocessor expands `offsetof(S, b)` to `((size_t)
&((struct S *)0)->b)`.  The parser:

1. `cast`: parens contain `(struct S *)0` cast.  Result:
   `ND_CAST(ND_NUM(0), pointer-to-struct-S)`.
2. `postfix` after `&...`: actually the `&` is part of the
   address-of unary.  Let's trace from outside:
   - `unary` sees `&`, then recursively parses `((struct S
     *)0)->b`:
     - `cast` returns the cast.
     - `postfix` sees `->`: `ND_DEREF`, then `struct_ref` for
       `b`.  Result: `ND_MEMBER(ND_DEREF(cast), b)`.
   - Wrap in `new_unary(ND_ADDR, ...)`.
3. `eval2` on `ND_ADDR(ND_MEMBER(ND_DEREF(ND_CAST(0, S*)),
   b))`:
   - `ND_ADDR` → `eval_rval` on operand.
   - `eval_rval(ND_MEMBER)` → `eval_rval(lhs) +
     member->offset`.
   - `eval_rval(ND_DEREF)` → `eval2(lhs)` with the same label
     slot.
   - `eval2(ND_CAST(NUM 0, S*))` → `eval2(NUM 0)` → 0 (no label).
   - Sum: `0 + b->offset = 4`.
4. Outer `(size_t)` cast preserves the value: 4.

### O.4 `&&label` in computed goto

```c
void *targets[] = { &&L1, &&L2 };
goto *targets[i];
```

For each `&&L1`:
1. `unary` sees `&&`, builds `ND_LABEL_VAL` with `label = "L1"`,
   `unique_label = "_cg_42"` (or shared from prior reference).
2. Prepended to `gotos` chain.

For `goto *expr`:
- `04c_stmt.md` §E.1 produces `ND_GOTO_EXPR` with `lhs = expr`.
- Codegen walks `gotos` to find every `ND_LABEL_VAL`'s
  `unique_label` and emits the dispatch code.

### O.5 Vector element-wise add

`vec_t a = b + c` where `vec_t = __attribute__((vector_size
(16))) int` (4 elements).

`new_add` sees both operands as `TY_VECTOR`, dispatches to
`vec_binary_op(ND_ADD, b, c, tok)`:

1. Allocate `tmp` of `vec_t`.
2. For `i = 0..3`:
   - `vec_set_elem(tmp, i, ND_ADD(vec_elem(b, i),
     vec_elem(c, i)))`.
3. Return comma-chain of 4 stores plus `tmp`.

Each `vec_elem(v, i)` is `*((int *)&v + i)` — codegen sees
scalar loads, no vector instruction.

---

## P. Phase 5 prerequisites added by 04b

Append to `04_parse.md` §15 master list during impl review:

1. **`Node->func_ty`, `Node->args`, `Node->lhs`, `Node->
   funcname` for `ND_FUNCALL`.**  Codegen lowers function
   calls using these four fields exclusively.

2. **`Node->cond`, `Node->then`, `Node->els` for `ND_COND`.**
   Codegen emits a branch on `cond`, materializes either
   `then` or `els`.

3. **`Node->body` for `ND_STMT_EXPR`.**  Codegen iterates the
   statement chain, then takes the value of the last
   `ND_EXPR_STMT`'s `lhs`.

4. **`Node->member` for `ND_MEMBER`.**  Codegen computes
   `lhs_addr + member->offset`, with bit-field load/store using
   `member->bit_offset` and `member->bit_width`.

5. **`Node->label`, `Node->unique_label` for `ND_LABEL_VAL`.**
   Codegen emits an instruction taking the address of the
   labeled location (the unique_label is what gets emitted as
   the `.global` symbol name).

6. **`Node->kind == ND_FRAME_ADDR`** for nested-function chain
   passing.  Codegen materializes the current frame pointer.

7. **The `*_overflow` Node kinds** carry the result-pointer
   argument as a separate `Node` field; codegen emits the
   write to that pointer plus the bool return.

8. **`ND_BUILTIN_ALLOCA`** with `ty = void *`: codegen emits
   stack adjustment + pointer return.  See also `04a_decl.md`
   §H.5 for the `ND_VLA_PTR` flow.

9. **`ND_BUILTIN_SETJMP` / `ND_BUILTIN_LONGJMP`**: codegen
   emits direct stack manipulation, not a libcall.

10. **`__va_area__`** is the variadic argument area object;
    `current_fn->va_area` is the `Obj` reference.  Codegen
    must emit prologue setup that places stack-spilled variadic
    args at this address.

---

## Q. Open questions

### Q.1 Common-type rule in `new_sub` for complex

`new_add`'s complex-on-complex case picks the wider base
(promoting `float` → `double` if either is `double`, etc.).
`new_sub`'s complex-on-complex case takes the lhs type
unconditionally (`Type *cty = is_complex(lhs->ty) ? lhs->ty :
rhs->ty`).

This asymmetry is observable: `(_Complex float)x - (_Complex
double)y` should produce `_Complex double` per C11
§6.3.1.8/1, but `new_sub` produces `_Complex float`.

Disposition: track during impl; either fix `new_sub` to match
`new_add` or document the divergence in §13 of `04_parse.md`.

### Q.2 `_Alignof` on expression form

C11 §6.5.3.4 specifies `_Alignof ( type-name )` only.  ncc
accepts `_Alignof unary-expr` as a GCC extension.  This is
recorded but not formalized in the spec.

Disposition: explicitly mark as accepted-extension in §13 of
`04_parse.md` to avoid spec drift.  Code paths that test the
extension are in the GCC torture suite, which we run and pass.

### Q.3 Implicit function declaration's variadic detection

`funcall` (§H.5) special-cases six libc functions (`printf`,
`fprintf`, `sprintf`, `snprintf`, `scanf`, `sscanf`) as
implicitly variadic.  Other variadic libc functions (`syslog`,
`execlp`, `open` with mode, etc.) are treated as non-variadic
on implicit declaration.

This is correct for the test corpus (which always declares
those functions explicitly via headers), but a spec ambiguity:
should the list be exhaustive, or stripped down, or extended?

Disposition: keep as-is.  Real source code includes proper
headers; the special case exists for tiny test programs and
the bootstrap path.

### Q.4 `eval2`'s `ND_VAR` anonymous-compound-literal path

The dig-into-relocations branch (§K.6) is targeted at one
specific kernel pattern.  It assumes a particular layout
(prefix `.L.data.`, single relocation at offset 0, etc.) that
might not generalize.

Disposition: keep as-is.  Pattern is well-documented; if
broader cases arise they get spec entries in §13 of
`04_parse.md`.
