# ncc Type System — English Specification (Phase 3)

This document is the behavioral contract for ncc's type system on
the `swap-out` branch.  Phase 3 of the chibicc swap-out replaces
the inherited `src/type.c` (435 lines, chibicc lineage) with a
reimplementation written from this document.  Both the current
code and the reimplementation must satisfy the contract;
mechanical validation (`scripts/bootstrap_validate.sh`, the gcc
torture suite, real-program builds) verifies behavioral
equivalence.

This is not a from-scratch C type system design — that work lives
in a separate project.  Here, the goal is byte-for-byte behavioral
equivalence with `main`'s type system, achieved through a
spec-mediated rewrite that removes chibicc-lineage code from
`src/`.

When the implementation and this document disagree, the document
is authoritative; bugs in the implementation are fixed against the
document.  When the document and `main`'s observable behavior
disagree, this document is wrong and must be updated, unless the
divergence is recorded in §10 as intentional.

**Phase 3 scope:** the type-system module (`src/type.c`).  Type-
related code in other modules (parser-side declarator parsing,
codegen-side ABI lowering, etc.) is owned by their respective
phases (4 and 5).

**Current status (2026-05-04):** §1–§12 drafted.  Phase 3
implementation work begins after this spec is reviewed.

---

## 1. Scope

The type system provides:
1. A small set of **predefined Type singletons** for the primitive
   C types (void, bool, signed/unsigned char/short/int/long/long
   long, float, double, long double).  Singleton identity is
   load-bearing: callers identity-compare these pointers.
2. **Type kind predicates** (`is_integer`, `is_flonum`, etc.).
3. **Type construction factories** for derived types (pointers,
   arrays, VLAs, enums, structs, functions, vectors, _Complex).
4. **C11-compatible-types check** (`is_compatible`, per §6.2.7).
5. **Usual arithmetic conversions** (`get_common_type`,
   `usual_arith_conv`, per §6.3.1.8) — used by `add_type` to
   insert implicit casts on binary operands.
6. **AST type derivation** (`add_type`): walks an AST and assigns
   each Node its result type per C semantics.

**Inputs:** `Type *` and `Node *` values from the rest of the
compiler.  No file I/O.  No mutable global state beyond the
file-scope Type singletons.

**Outputs:** new `Type *` values (constructed) and `node->ty`
field assignments (mutated in place).

**Module discipline:** the type system is essentially a pure
function library.  All entry points are direct function calls;
no callbacks, no thread-local state.

---

## 2. The `Type` data structure

`struct Type` is defined in `cc.h`.  The fields the type system
reads or writes:

| Field | Type | Purpose |
|---|---|---|
| `kind` | `TypeKind` enum | the discriminator (see §3) |
| `size` | `int` | byte size for sizeof; -1 for incomplete types |
| `align` | `int` | byte alignment |
| `is_unsigned` | `bool` | for integer kinds only |
| `is_variadic` | `bool` | for `TY_FUNC` only |
| `base` | `Type *` | element type for ptr/array/vla/vector/complex |
| `array_len` | `long` | array length for `TY_ARRAY`; element count for `TY_VECTOR` (Q4: overload, see §4.5); `-1` = unspecified |
| `vla_len` | `Node *` | runtime expression for `TY_VLA` |
| `return_ty` | `Type *` | return type for `TY_FUNC` |
| `params` | `Type *` | parameter list for `TY_FUNC` (linked via `next`) |
| `next` | `Type *` | next link in a parameter list; reset to NULL by `copy_type` (§7.1, Q3) |
| `origin` | `Type *` | typedef back-pointer set by `copy_type`; consulted by `is_compatible` (§6) |
| `members` | `Member *` | member list for `TY_STRUCT`/`TY_UNION`; not read by type.c (set/read by parse.c) |
| `tag` | `Token *` | tag token for `TY_STRUCT`/`TY_UNION`; not read by type.c |

The fields not listed here (e.g., parser-internal flags) are
outside Phase 3 scope.

---

## 3. `TypeKind` enum

The discriminator for `struct Type`.  Defined in `cc.h`; the type
system handles every kind that comes up in the AST.

| Kind | Category |
|---|---|
| `TY_VOID` | scalar (size 1; for void-pointer arithmetic) |
| `TY_BOOL` | scalar (unsigned, size 1) |
| `TY_CHAR` | scalar (signed by default; `is_unsigned` flag for unsigned char) |
| `TY_SHORT` | scalar |
| `TY_INT` | scalar |
| `TY_LONG` | scalar |
| `TY_LONGLONG` | scalar |
| `TY_FLOAT` | scalar (IEEE 754 single) |
| `TY_DOUBLE` | scalar (IEEE 754 double) |
| `TY_LDOUBLE` | scalar (Apple ARM64: same as double — see §10, Q10) |
| `TY_ENUM` | scalar (size 4) |
| `TY_PTR` | derived (8-byte pointer) |
| `TY_ARRAY` | derived (`base` × `array_len`) |
| `TY_VLA` | derived (runtime-sized; stored as 8-byte pointer) |
| `TY_FUNC` | derived (incomplete; `return_ty` + `params`) |
| `TY_STRUCT` | derived (members) |
| `TY_UNION` | derived (members) |
| `TY_COMPLEX` | derived (`base` × 2 — C11 `_Complex`) |
| `TY_VECTOR` | derived (`__attribute__((vector_size(N)))`) |

---

## 4. Predefined Type singletons

These are file-scope compound literals at fixed addresses
(C99/C11 §6.5.2.5: a compound literal at file scope has static
storage duration).  Their addresses are referenced by name from
many modules and are identity-compared in places (Q1 — load-
bearing invariant).

The Phase 3 reimplementation **must not**:
- Allocate fresh storage for these (e.g., `calloc_checked` then
  return) — that breaks identity-compare callers.
- Move them out of file scope into functions returning a fresh
  pointer.
- Change their addresses across translation units (they are
  unique per process).

| Name | Kind | Size | Align | Notes |
|---|---|---|---|---|
| `ty_void` | TY_VOID | 1 | 1 | size 1 enables void-pointer arithmetic |
| `ty_bool` | TY_BOOL | 1 | 1 | `is_unsigned = true` |
| `ty_char` | TY_CHAR | 1 | 1 | signed (aarch64 default) |
| `ty_short` | TY_SHORT | 2 | 2 | signed |
| `ty_int` | TY_INT | 4 | 4 | signed |
| `ty_long` | TY_LONG | 8 | 8 | signed |
| `ty_longlong` | TY_LONGLONG | 8 | 8 | signed |
| `ty_uchar` | TY_CHAR | 1 | 1 | `is_unsigned = true` |
| `ty_ushort` | TY_SHORT | 2 | 2 | `is_unsigned = true` |
| `ty_uint` | TY_INT | 4 | 4 | `is_unsigned = true` |
| `ty_ulong` | TY_LONG | 8 | 8 | `is_unsigned = true` |
| `ty_ulonglong` | TY_LONGLONG | 8 | 8 | `is_unsigned = true` |
| `ty_float` | TY_FLOAT | 4 | 4 | IEEE 754 single |
| `ty_double` | TY_DOUBLE | 8 | 8 | IEEE 754 double |
| `ty_ldouble` | TY_LDOUBLE | 8 | 8 | **8** on Apple ARM64 (Q10) |

Implementation pattern (preserved verbatim from `main`'s
behavior):

```c
Type *ty_void = &(Type){TY_VOID, 1, 1};
Type *ty_int  = &(Type){TY_INT,  4, 4};
Type *ty_uint = &(Type){TY_INT,  4, 4, .is_unsigned = true};
/* ... etc. */
```

The compound literals are positional (kind, size, align) plus
designated `.is_unsigned` for unsigned variants.

These are declared `extern` in `cc.h`; defined in `type.c`.

---

## 5. Type kind predicates

Pure boolean tests on `ty->kind` (and sometimes `ty->base`).
Trivial; implementation should be as direct as the kind table.

| Predicate | Returns true for |
|---|---|
| `is_integer(ty)` | `kind` ∈ {BOOL, CHAR, SHORT, INT, LONG, LONGLONG, ENUM} |
| `is_flonum(ty)` | `kind` ∈ {FLOAT, DOUBLE, LDOUBLE} |
| `is_numeric(ty)` | `is_integer(ty) || is_flonum(ty)` |
| `is_complex(ty)` | `kind == TY_COMPLEX` |
| `is_vector(ty)` | `kind == TY_VECTOR` |

Note: `TY_ENUM` counts as integer (its underlying representation
is `int`).  `TY_BOOL` counts as integer (it's a 1-byte integer).
`TY_COMPLEX` does **not** count as numeric (per the predicate
table) — `is_complex` is separate.

---

## 6. Type compatibility — `is_compatible(t1, t2)`

C11 §6.2.7 type-compatibility check.  Returns `true` if the two
types are compatible per the standard, `false` otherwise.

### 6.1 Algorithm

1. **Pointer-equality fast path.** If `t1 == t2`, return `true`.
2. **Typedef transparency.** If `t1->origin != NULL`, recurse:
   `is_compatible(t1->origin, t2)`.  Same for `t2->origin`.  This
   walks through any `copy_type`-induced typedef chain.
3. **Same-kind requirement.** If `t1->kind != t2->kind`, return
   `false`.
4. **Per-kind logic** (switch on `t1->kind`):
   - **Integer** (CHAR, SHORT, INT, LONG, LONGLONG): same kind
     plus `is_unsigned == is_unsigned`.
   - **Float** (FLOAT, DOUBLE, LDOUBLE): same kind.
   - **TY_PTR**: `is_compatible(t1->base, t2->base)`.
   - **TY_FUNC**: `is_compatible(return_ty, return_ty)`,
     `is_variadic == is_variadic`, walk `params` lists in
     lockstep, each pair `is_compatible`, both lists must end at
     the same time.
   - **TY_ARRAY**: `is_compatible(base, base)`, length matches OR
     either is unspecified (`-1`).
   - **TY_VECTOR**: same `size` and base compatible.
   - **default** (struct, union, enum, void, bool — anything not
     covered above): `false` (the pointer-equality fast path at
     step 1 handles same-instance compatibility for these).

### 6.2 Recursion vs iteration (Q2)

The implementation is recursive on `origin`, `base`, and `params`.
On the in-scope corpus, typedef chains are bounded (~5 levels
max) and base/param chains are bounded by C's nesting limits.  A
deduplicating implementation could be iterative, but the
recursive form is clearer and small enough that the spec
preserves it.

### 6.3 Pointer-equality fast path

Two Type values that compare equal as pointers are necessarily
compatible.  The fast path at step 1 saves significant work for
the predefined singletons (most uses of `ty_int` flow through the
same singleton, so identity-compare succeeds).

---

## 7. Type construction factories

Each factory `calloc_checked(1, sizeof(Type))`s a fresh Type and
fills in the kind-specific fields.  Returns the new Type.

### 7.1 `copy_type(Type *ty)`

Shallow-copy a Type and set up typedef back-tracking.

```
ret = calloc_checked(1, sizeof(Type));
*ret = *ty;
ret->origin = ty;
ret->next = NULL;
return ret;
```

**Two non-obvious bits** (Q3):
- `origin = ty` makes the copy a typedef-style alias of the
  original.  `is_compatible` walks `origin` so the copy
  type-matches the original.
- `next = NULL` is a deliberate reset.  `copy_type` is for
  single-Type copies (e.g., a parameter type), not for list
  nodes.  If the source's `next` were preserved, the copy would
  silently inherit a list link that doesn't belong to it.  The
  spec explicitly forbids preserving `next`.

### 7.2 `pointer_to(Type *base)`

```
kind = TY_PTR
size = 8           # 64-bit pointers on aarch64 macOS
align = 8
base = base
```

### 7.3 `func_type(Type *return_ty)`

```
kind = TY_FUNC
return_ty = return_ty
# size and align left at 0 — functions are incomplete types,
# always used via pointer.
```

### 7.4 `array_of(Type *base, long len)`

```
kind = TY_ARRAY
size = base->size * len
align = base->align
base = base
array_len = len
```

For unspecified-length arrays (e.g., `int a[]`), the parser
passes `len = -1`; size will be incorrect but downstream callers
gate on the unspecified case.

### 7.5 `vla_of(Type *base, Node *len)`

```
kind = TY_VLA
size = 8           # VLA is internally a pointer
align = 8
base = base
vla_len = len      # runtime-evaluated expression
```

### 7.6 `enum_type(void)`

```
kind = TY_ENUM
size = 4
align = 4
```

ncc's enums are `int`-sized regardless of value range (a known
simplification — see §10).

### 7.7 `struct_type(void)`

```
kind = TY_STRUCT
# size and align stay 0; parse.c fills them in after laying out members.
```

### 7.8 `vector_of(Type *base, int total_size)`

```
kind = TY_VECTOR
base = base
size = total_size
align = min(total_size, 16)   # cap to avoid excessive stack alignment
array_len = total_size / base->size   # element count (Q4: overloaded field)
```

The `array_len` overload (element count for vectors, array length
for arrays) is preserved per Q4.  The spec describes the semantic;
implementation may use a separate field if desired, but the
external behavior must match.

### 7.9 `complex_type(Type *base)`

```
kind = TY_COMPLEX
base = base
size = base->size * 2     # _Complex T = two Ts
align = base->align
```

---

## 8. Usual arithmetic conversions

Per C11 §6.3.1.8.  Two functions:

### 8.1 `get_common_type(Type *ty1, Type *ty2)` (static)

Returns the common type for a binary operation.  Priority order
(first match wins):

1. **Vector wins.**  If either operand is `TY_VECTOR`, return that
   type as-is.  Vector-vs-vector or vector-vs-scalar produces a
   vector result.
2. **Pointer-via-base.** If `ty1->base != NULL` and `ty1->kind !=
   TY_COMPLEX`, return `pointer_to(ty1->base)`.  This handles
   pointer/array/VLA on ty1.  (Note the priority: vector beats
   this; a vector also has a `base` but is handled at step 1.)
3. **Complex wins.** If either operand is `TY_COMPLEX`, recurse
   on the bases and wrap the result with `complex_type(...)`.
4. **Function-to-pointer decay.** If either operand is `TY_FUNC`,
   return `pointer_to(thatType)`.
5. **Float promotion ladder.** ldouble > double > float.
6. **Integer promotion.** Operands smaller than `int` (size < 4)
   are promoted to `ty_int` first.
7. **Larger size wins.** If sizes differ, the larger type wins.
8. **Unsigned wins.** Same size, unsigned wins.

The order matters: vector beats complex beats float beats integer.

### 8.2 `usual_arith_conv(Node **lhs, Node **rhs)` (static)

Insert implicit casts on both operands of a binary expression so
they share the common type:

```
ty = get_common_type((*lhs)->ty, (*rhs)->ty);
*lhs = new_cast(*lhs, ty);
*rhs = new_cast(*rhs, ty);
```

`new_cast` is parse.c's helper.  Type system depends on it.

### 8.3 Vector arithmetic exception

Vector arithmetic does **not** go through `usual_arith_conv`.  The
type-derivation path for vector operands sets the result type
directly to the vector type (see §9 dispatch table).  The `new_cast`
machinery would coerce away vector-ness, which is wrong.

---

## 9. `add_type(Node *node)` — type derivation across the AST

Walks an AST top-down and assigns `node->ty` per the node's kind.

### 9.1 Idempotency (Q5)

`add_type` checks `node->ty` early and bails if already set:

```
if (!node || node->ty)
    return;
```

This is load-bearing.  parse.c calls `add_type` speculatively in
several places.  A non-idempotent implementation would silently
recompute (and possibly change) types that callers already
treated as final.

The spec mandates idempotency.  The reimplementation must
preserve the early-bail check.

### 9.2 Recursive walk

Before applying any kind-specific rule, recursively type all
child Nodes:

```
add_type(node->lhs);
add_type(node->rhs);
add_type(node->cond);
add_type(node->then);
add_type(node->els);
add_type(node->init);
add_type(node->inc);
for (Node *n = node->body; n; n = n->next) add_type(n);
for (Node *n = node->args; n; n = n->next) add_type(n);
```

Then dispatch on `node->kind`.

### 9.3 Per-kind dispatch table

| Kind | Rule |
|---|---|
| `ND_NUM` | If `ty` not already set, default to `ty_int`. |
| `ND_ADD`/`ND_SUB`/`ND_MUL`/`ND_DIV`/`ND_MOD`/`ND_BITAND`/`ND_BITOR`/`ND_BITXOR` | Vector-aware (vector wins, no usual-arith); else `usual_arith_conv`, then `node->ty = node->lhs->ty`. |
| `ND_NEG` | Vector-aware; integer promotion to `ty_int` if base size < 4; else `node->ty = node->lhs->ty`. |
| `ND_ASSIGN` | Error if lhs is `TY_ARRAY` (not lvalue). Cast rhs to lhs type unless lhs is struct/complex/vector. `node->ty = node->lhs->ty`. |
| `ND_EQ`/`ND_NE`/`ND_LT`/`ND_LE` | Vector-aware (result is signed vector); else `usual_arith_conv`, then `node->ty = ty_int`. |
| `ND_FUNCALL` | `node->ty = node->func_ty->return_ty`. |
| `ND_NOT`/`ND_LOGAND`/`ND_LOGOR` | `node->ty = ty_int`. |
| `ND_BITNOT`/`ND_SHL`/`ND_SHR` | Vector-aware; integer promotion of lhs; `node->ty = lhs->ty`. |
| `ND_VAR`/`ND_VLA_PTR`/`ND_CHAIN_VAR` | `node->ty = node->var->ty`; if VLA, decay to `pointer_to(base)`. |
| `ND_COND` | Void-aware (any branch void → result void); else `usual_arith_conv` on then/els; `node->ty = node->then->ty`. |
| `ND_COMMA` | `node->ty = node->rhs->ty`. |
| `ND_MEMBER` | `node->ty = member->ty`; bitfield promotion (Q7, see §9.4). |
| `ND_ADDR` | If lhs is `TY_ARRAY`, `pointer_to(lhs->ty->base)`; else `pointer_to(lhs->ty)`. |
| `ND_DEREF` | If lhs is `TY_FUNC`, `node->ty = lhs->ty` (Q9: typeof support); else error if `!lhs->ty->base`; else `node->ty = lhs->ty->base`. VLA stays VLA (Q6). |
| `ND_STMT_EXPR` | Type of last statement if it's an expression statement; else `ty_void`. |
| `ND_LABEL_VAL` | `pointer_to(ty_void)` (GCC `&&label`). |
| `ND_FRAME_ADDR` | `pointer_to(ty_char)`. |
| `ND_RETURN_ADDR`/`ND_BUILTIN_FRAME_ADDR` | `pointer_to(ty_void)`. |
| `ND_TRAMPOLINE` | No-op (caller already set ty). |
| `ND_REAL`/`ND_IMAG` | If `lhs->ty` is `TY_COMPLEX`, `node->ty = lhs->ty->base`; else `node->ty = lhs->ty` (Q8: identity on non-complex). |
| `ND_CAS` | Recursively type `cas_addr`/`cas_old`/`cas_new`; `node->ty = ty_bool`. |
| `ND_EXCH` | Recursively type `cas_addr`/`cas_new`; `node->ty = cas_addr->ty->base`. |
| **default** | No-op.  Many declaration / control-flow nodes hit this case. |

### 9.4 Bitfield promotion (Q7)

For `ND_MEMBER` on a bitfield member (`node->member->is_bitfield`),
the result type promotes per C standard's "value preserving" rule:

| Bitfield base type | Bit width | Promoted type |
|---|---|---|
| Unsigned | < 32 | `ty_int` (signed!) — the int can hold all unsigned values < 32 bits |
| Unsigned | == 32 | `ty_uint` |
| Signed | ≤ 32 | `ty_int` |

The spec preserves this exactly — it is C-standard-mandated.  A
worked example regression test pins the rule (`tests/regression/
NN_bitfield_promotion.c` per Q11.B).

### 9.5 VLA decay deferral (Q6)

`ND_DEREF` on a `TY_VLA` operand sets `node->ty` to the VLA type
itself (not `pointer_to(base)`).  The decay to pointer happens in
parse.c's `new_add` / `new_sub` when the VLA is used in pointer
arithmetic, not in `add_type`.

This is a division of responsibility between type.c and parse.c.
Phase 4 (parser swap-out) must preserve the same division.

### 9.6 ND_DEREF on functions (Q9)

`*(func_ptr)` where the operand has `TY_FUNC`: `node->ty =
node->lhs->ty` (returns the function type, not the return type).
This supports the GCC `typeof(*func)` extension.

---

## 10. Known divergences from ISO C / GCC

Behaviors preserved for compatibility with `main`'s type system,
documented here so a re-implementer does not "fix" them by
accident.

- **`long double` size = 8** on Apple ARM64 (Q10).  C standard
  permits any size ≥ `double`'s size; Apple's ARM64 ABI sets
  `long double` to 8 bytes.  Differs from x86_64 (16) and
  PowerPC (12).  Hard-coded for Phase 3 scope; Phase 5 (codegen
  audit) is the natural place to introduce target
  parameterization.

- **Enums always 4 bytes** regardless of value range.  C standard
  permits implementations to pick the smallest sufficient
  integer type; ncc always uses `int`-sized.  Real corpus relies
  on this implicitly (functions that return enum and integer
  interchangeably); changing it would be a behavior change.

- **`__real__(x)` / `__imag__(x)` on non-complex `x`** — `add_type`
  sets `node->ty = node->lhs->ty` for both (identity).  This
  matches GCC's documented behavior: `__real__` is identity,
  `__imag__` is zero-typed-as-x.  No special casing in `add_type`
  — both produce the same type.

- **`*(func)` returns the function type** (not the return type).
  GCC `typeof` extension; supports patterns like `typeof(*malloc)
  *fn`.

- **`*(void *)p` permitted with type `void`.**  `add_type` doesn't
  reject the dereference; subsequent operations handle the void
  type appropriately.  Used by `__is_constexpr`-style probes.

- **VLA-of-pointer-decay deferred to parse.c.**  `add_type` on
  `ND_DEREF` of VLA leaves the type as VLA; the decay to pointer
  happens in `new_add`/`new_sub`.  Asymmetric vs regular array
  deref (which does decay).  Phase 4 must preserve this.

- **Bitfield promotion sign-asymmetry.**  An unsigned bitfield of
  width < 32 promotes to `ty_int` (signed).  C-standard mandate;
  not a divergence per se but worth flagging because the table
  looks counterintuitive.

- **Singletons at fixed addresses.**  `ty_int`, `ty_void`, etc.
  must remain at file-scope compound-literal addresses.  Some
  callers identity-compare these pointers; recreating them as
  fresh allocations would silently break those callers.

---

## 11. Validation criteria for the swap-out implementation

A re-implementation of the type system derived from this spec is
correct when all of the following hold:

1. **Bootstrap fixed-point** (`scripts/bootstrap_validate.sh`):
   stage1 == stage2 by md5.  Type system bugs typically
   manifest as silent miscompilations that show up at the second
   bootstrap stage.
2. **Torture parity**: `tests/torture/run.sh` reports the same
   `PASS=` count as the `phase-2-closed` baseline (964/995, 100%
   on non-skip).
3. **Regression suite**: `tests/regression/run.sh` reports
   `PASS=N+ FAIL=0`, where N is the count at `phase-2-closed` (23)
   plus any new Phase-3 tests added per Q11.
4. **Real-program builds**: `tests/sqlite/build.sh`,
   `build_doom_ncc2.sh`, `tests/cpython/build.sh` all succeed
   matching the `phase-2-closed` baseline numbers.
5. **Tokenizer + preprocessor harnesses** (`scripts/validate_*.sh`):
   unchanged from `phase-2-closed`.

Type-system bugs are usually catastrophic (wrong code generated)
rather than subtle, so failing the bootstrap fixed-point is the
fastest detector.  Real-program builds are the broadest detector.

A failure on any of these reverts the swap-out commit per the
phase-discipline rule.

---

## 12. Out of scope

The following are **not** part of Phase 3:

- **Declarator parsing** (Phase 4).  `parse.c` is responsible for
  building Type values from C declarator syntax; type.c receives
  fully-formed Types.
- **Codegen ABI details** (Phase 5).  How a Type is laid out in
  registers, how struct args are passed, etc., live in
  `codegen_arm64.c`.  type.c provides size/align; codegen
  consumes them.
- **Target portability.**  Phase 3 hard-codes the aarch64 macOS
  ABI (LP64, ldouble = 8, char = signed, etc.).  Multi-target
  parameterization is Phase 5/6 work.
- **`<stdint.h>` / `<stddef.h>` types** (e.g., `size_t`).  These
  are exposed via the preprocessor's `__SIZE_TYPE__` etc.
  predefines and are typedef'd in their respective system
  headers.  The type system sees them after typedef resolution
  via `copy_type` + `origin`.
- **Type qualifiers** (`const`, `volatile`, `restrict`, `_Atomic`).
  ncc treats these as parser-side noise that doesn't affect
  observable behavior.  No Type field tracks them; they don't
  appear in `is_compatible`.
- **`<stdatomic.h>` / `_Atomic`-typed variables.**  ncc advertises
  `__STDC_NO_ATOMICS__ = 1`.  Atomic types are not part of the
  type system.
