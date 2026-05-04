# ncc Type System — Phase 3 Inventory

This is a read-only audit of `src/type.c` (435 lines) on the
`swap-out` branch (tip `4cd431d`, post `phase-2-closed`).  Inventory,
not spec.  The spec (`03_type.md`) is the user-collaborative next
step that comes after this document.

Phase 3 scope: replace `src/type.c` with a spec-derived
implementation under the same chunked / dual-build / no-peek
discipline established in Phase 2.

The type system is small (~3× smaller than the preprocessor) and
mostly self-contained: it operates on `struct Type` (defined in
`cc.h`) and the parser's `struct Node` (also in `cc.h`).  No
preprocessor or directive handling.  No file I/O.  Most of the
work is type construction (object factories) and type derivation
(walking an AST and inferring node types).

---

## 1. Function inventory

### Public (declared in `cc.h`)

| Function | Line | Purpose |
|---|---|---|
| `is_integer(Type *ty)` | 27 | True if `ty->kind` is one of {BOOL, CHAR, SHORT, INT, LONG, LONGLONG, ENUM}. |
| `is_flonum(Type *ty)` | 34 | True if `ty->kind` is one of {FLOAT, DOUBLE, LDOUBLE}. |
| `is_numeric(Type *ty)` | 39 | `is_integer(ty) \|\| is_flonum(ty)`. |
| `is_complex(Type *ty)` | 43 | `ty->kind == TY_COMPLEX`. |
| `is_compatible(Type *t1, Type *t2)` | 47 | C11-compatible-types check; recursive over base / params / array length / vector shape; tracks `origin` chain for typedef compatibility. |
| `copy_type(Type *ty)` | 96 | Shallow copy of a Type, sets `origin = ty` (typedef back-pointer), `next = NULL`. |
| `pointer_to(Type *base)` | 104 | Construct a `TY_PTR` Type with size 8, align 8, `base = base`. |
| `func_type(Type *return_ty)` | 113 | Construct a `TY_FUNC` Type (incomplete; not sized) with `return_ty`. |
| `array_of(Type *base, long len)` | 122 | Construct a `TY_ARRAY` with `size = base->size * len`. |
| `vla_of(Type *base, Node *len)` | 132 | Construct a `TY_VLA` with size 8, align 8 (VLA stored as pointer to base), `vla_len = len` (a Node, runtime-evaluated). |
| `enum_type(void)` | 142 | Construct a `TY_ENUM` with size 4, align 4. |
| `struct_type(void)` | 150 | Construct a `TY_STRUCT` with default size/align (filled in later by parser). |
| `vector_of(Type *base, int total_size)` | 158 | Construct a `TY_VECTOR` with `size = total_size`, `align = min(total_size, 16)`, `array_len = total_size / base->size` (element count reuses array_len). |
| `is_vector(Type *ty)` | 171 | `ty->kind == TY_VECTOR`. |
| `complex_type(Type *base)` | 177 | Construct a `TY_COMPLEX` with `size = base->size * 2`, `align = base->align`, `base = base`. |
| `add_type(Node *node)` | 243 | The largest function (~190 lines).  Top-down recursive: walks all child Nodes, then applies a per-node-kind type-derivation rule.  Idempotent (returns immediately if `node->ty` is already set). |

### Module-internal (`static`)

| Function | Line | Purpose |
|---|---|---|
| `get_common_type(Type *ty1, Type *ty2)` | 187 | Compute the common type for binary operations (C11 usual arithmetic conversions): vector wins, complex wins, ldouble > double > float > integer-promoted to int > larger size > unsigned. |
| `usual_arith_conv(Node **lhs, Node **rhs)` | 236 | Insert implicit casts on both operands of a binary expression so they share the common type computed by `get_common_type`. |

---

## 2. Type system responsibilities by topic

### 2.1 Predefined Type globals

`type.c` lines 8–25 define static `Type` structs for the primitive
types, addressable as `ty_void`, `ty_bool`, `ty_char` … `ty_ldouble`,
plus unsigned variants.  These use C99 compound-literal syntax with
designated initializers (`= &(Type){TY_VOID, 1, 1}`).  The `&`
captures the address of a compound literal at file scope; this is
specified by C99 §6.5.2.5 to have static storage duration.

The sizes/aligns hard-code aarch64 macOS:
- `char` = 1, `short` = 2, `int` = 4, `long` = 8, `long long` = 8
- `float` = 4, `double` = 8, **`long double` = 8** (Apple ARM64 ABI;
  not the 16-byte ldouble of x86_64 or PowerPC)

### 2.2 Type kinds (TypeKind enum in cc.h)

Read by `type.c` but defined in `cc.h`:
- Scalar: `TY_VOID`, `TY_BOOL`, `TY_CHAR`, `TY_SHORT`, `TY_INT`,
  `TY_LONG`, `TY_LONGLONG`, `TY_FLOAT`, `TY_DOUBLE`, `TY_LDOUBLE`
- Compound: `TY_ENUM`, `TY_PTR`, `TY_ARRAY`, `TY_VLA`, `TY_FUNC`,
  `TY_STRUCT`, `TY_UNION`
- Extensions: `TY_COMPLEX` (C11 `_Complex`), `TY_VECTOR`
  (`__attribute__((vector_size(N)))`)

### 2.3 Object factories (lines 96–184)

Each constructor allocates via `calloc_checked(1, sizeof(Type))`,
sets the kind + size + align + base/return_ty/array_len/vla_len/etc
fields specific to that kind, and returns the new Type.

`copy_type` is the typedef helper — produces a fresh Type whose
`origin` field points back at the source Type.  Used by `parse.c`
when building typedef'd types.

### 2.4 `is_compatible` — C11 type-compatibility check

`type.c:47–94` implements C11 §6.2.7 type compatibility:

1. Pointer-equality fast path.
2. Walk `origin` chain on both sides (typedef transparency).
3. Same-kind requirement.
4. Per-kind logic:
   - Integer types: same kind + same `is_unsigned`.
   - Float types: same kind.
   - Pointer: `is_compatible(base, base)` recursively.
   - Function: same `return_ty`, same `is_variadic`, same param
     count, each param `is_compatible`.
   - Array: same base; array length matches OR either is unspecified
     (-1).
   - Vector: same total size + base compatible.
   - Other (struct, union, enum, etc.): only compatible to self
     (handled by the pointer-equality fast path above).

### 2.5 Usual arithmetic conversions

`get_common_type` + `usual_arith_conv` implement C11 §6.3.1.8.  The
order is: vector > complex > pointer (when one side has a non-COMPLEX
base) > function-to-pointer-decay > ldouble > double > float >
integer promotion to int > larger size > unsigned-wins.

`usual_arith_conv` is called from `add_type` for nodes that perform
binary arithmetic (ADD, SUB, MUL, DIV, MOD, BITAND, BITOR, BITXOR,
EQ, NE, LT, LE) and ternary (COND).

### 2.6 `add_type` — type derivation across the AST

`type.c:243–435` (about 190 lines).  Recursively types every node
in the AST.  Algorithm:

1. Bail if `node == NULL` or `node->ty` already set (idempotency).
2. Recursively type all child nodes (lhs, rhs, cond, then, els,
   init, inc, body list, args list).
3. Switch on `node->kind` and apply a kind-specific rule to compute
   `node->ty`.

Kind rules (selected highlights):
- **`ND_NUM`**: default to `ty_int` if not already set (the
  tokenizer / parser sets `ty` for typed numeric literals).
- **Binary arithmetic** (ADD/SUB/MUL/DIV/MOD/BITAND/BITOR/BITXOR):
  vector-aware (vector type wins; no usual-arith); else
  `usual_arith_conv` then take lhs type.
- **Comparison** (EQ/NE/LT/LE): vector-aware (result is signed
  vector); else usual-arith then result is `ty_int`.
- **`ND_NEG`**: integer promotion to `ty_int` if base is smaller.
- **`ND_ASSIGN`**: arrays not lvalues; cast rhs to lhs type unless
  lhs is struct/complex/vector.
- **`ND_FUNCALL`**: result is `node->func_ty->return_ty`.
- **`ND_NOT` / `ND_LOGAND` / `ND_LOGOR`**: result is `ty_int`.
- **`ND_BITNOT` / `ND_SHL` / `ND_SHR`**: integer promotion of lhs.
- **`ND_VAR` / `ND_VLA_PTR` / `ND_CHAIN_VAR`**: `node->var->ty`,
  with VLA-to-pointer decay.
- **`ND_COND`**: void-aware; usual-arith of then/els; result is then
  type.
- **`ND_COMMA`**: result is rhs type.
- **`ND_MEMBER`**: result is member type, with bitfield-promotion
  to int (signed/unsigned-aware based on bit-width).
- **`ND_ADDR`**: pointer-to-base for arrays; else pointer-to-lhs.
- **`ND_DEREF`**: function-to-pointer-decay (function returns its
  own type — GCC `typeof` extension); else `lhs->ty->base`.
- **`ND_STMT_EXPR`**: type of last expression statement; else void.
- **`ND_LABEL_VAL`** (GCC `&&label`): `void *`.
- **`ND_FRAME_ADDR`**: `char *`.
- **`ND_RETURN_ADDR` / `ND_BUILTIN_FRAME_ADDR`**: `void *`.
- **`ND_TRAMPOLINE`**: no-op (caller has already set ty).
- **`ND_REAL` / `ND_IMAG`** (`__real__` / `__imag__`): base of
  complex; else operand type (identity/zero for non-complex).
- **`ND_CAS` / `ND_EXCH`** (atomic compare-and-swap / exchange):
  result is `ty_bool` / `cas_addr->ty->base`.
- **default**: no-op.

Many node kinds (control-flow, declarations) hit the `default` case
because their type is set elsewhere or not meaningful.

---

## 3. Type field usage

`struct Type` is defined in `cc.h:425+`.  Fields read or written by
`type.c`:

| Field | Read | Written | Notes |
|---|---|---|---|
| `kind` | extensively (every constructor + every is_* + add_type switch) | every constructor | the discriminator |
| `size` | get_common_type, ND_NEG/SHL/SHR promotion, ND_NUM | every constructor | byte size |
| `align` | (not read in type.c — read by codegen + parse.c for layout) | every constructor | byte alignment |
| `is_unsigned` | is_compatible (integer arm), get_common_type (same-size tiebreaker), ND_MEMBER bitfield promotion | predefined globals (line 9, 17–21); inherited via copy_type | unsigned-ness flag |
| `is_variadic` | is_compatible (function arm) | not in type.c (set by parse.c) | function variadic flag |
| `is_atomic` | not in type.c | not in type.c | (may be referenced elsewhere) |
| `base` | extensively (get_common_type, is_compatible, ND_DEREF / ND_ADDR / ND_REAL / ND_IMAG) | constructors that have one | element type for ptr/array/vla/vector/complex |
| `array_len` | is_compatible (array arm) | array_of, vector_of (reuses array_len for element count) | array length, -1 = unspecified |
| `vla_len` | not read in type.c | vla_of | the runtime expression |
| `return_ty` | is_compatible (function arm), ND_FUNCALL | func_type | function return type |
| `params` | is_compatible (function arm) | not in type.c (set by parse.c) | function parameter list |
| `next` | is_compatible (function param walk) | reset to NULL by copy_type | linked-list link for params |
| `origin` | is_compatible (typedef transparency) | copy_type | typedef back-pointer |
| `members` (struct) | not read in type.c (read by parse.c) | not in type.c | |
| `tag` (struct/union) | not read in type.c | not in type.c | |

**Critical reliances:**
- `kind` must be set before any `is_*` check.
- Predefined Type globals (`ty_int` etc.) are file-scope compound
  literals with static storage duration.  Their addresses are
  shared widely across the compiler — never copy these or treat
  them as mutable.

---

## 4. Edge cases observed

Grep for flagged comments (`GCC`, `extension`, `typeof`, `subtle`,
`tricky`, `must`, `should`, `Note`, `TODO`, `FIXME`, `HACK`):

| Line | Keyword | Summary |
|---|---|---|
| 8 lines (8–25) | (file comment) | aarch64 macOS sizes hard-coded. |
| 25 | (inline) | `long double` = 8 on Apple ARM64 (vs 16 on x86_64 / PowerPC). |
| 156 | (comment) | `vector_of` reuses `array_len` field for element count. |
| 164 | (comment) | Vector alignment capped at 16 to avoid excessive stack alignment. |
| 175 | (comment) | `complex_type`: `_Complex double` = 16 bytes (two doubles). |
| 376 | `GCC typeof extension` | ND_DEREF on a function type yields the function type (not its return type) — supports the `typeof(*func)` pattern. |
| 383 | `GCC extension` | Allow `void` pointer dereference (`*(void *)p`); yields type `void`. Used by `__is_constexpr`-style probes. |
| 386–388 | (multi-line comment) | VLA types stay as VLA in ND_DEREF; load() will skip the load and VLA-to-pointer decay happens in `new_add`/`new_sub`. |
| 419 | (inline) | `__real__`/`__imag__` on non-complex is identity (and `__imag__` is zero) per GCC convention. |

No `TODO`/`FIXME`/`XXX`/`HACK` markers in `type.c`.

---

## 5. GCC/clang extensions used in `type.c` source

Per the project's pure-C constraint, the Phase 3 reimplementation
must use C11 only.  The following non-standard constructs appear in
`type.c`:

| Lines | Extension | C11 replacement |
|---|---|---|
| 8–25 | C99 compound literal at file scope `&(Type){...}` with designated initializers | C99-compliant; **no replacement needed** (compound literals are C99 standard, not a GCC extension; designated initializers are also C99 standard) |
| Throughout | none | — |

`type.c` is essentially clean C11.  No `__attribute__`, no
`__builtin_*`, no statement expressions, no nested functions, no
typeof, no labels-as-values.  This makes the Phase 3 reimplementation
the simplest of the swap-out so far on the pure-C dimension.

---

## 6. Predefined Type table

| Name | Kind | Size | Align | Notes |
|---|---|---|---|---|
| `ty_void` | TY_VOID | 1 | 1 | Size 1 chosen to allow void-pointer arithmetic. |
| `ty_bool` | TY_BOOL | 1 | 1 | unsigned. |
| `ty_char` | TY_CHAR | 1 | 1 | signed (the default on aarch64; clang differs on some targets). |
| `ty_short` | TY_SHORT | 2 | 2 | signed. |
| `ty_int` | TY_INT | 4 | 4 | signed. |
| `ty_long` | TY_LONG | 8 | 8 | signed. |
| `ty_longlong` | TY_LONGLONG | 8 | 8 | signed. |
| `ty_uchar` | TY_CHAR | 1 | 1 | unsigned. |
| `ty_ushort` | TY_SHORT | 2 | 2 | unsigned. |
| `ty_uint` | TY_INT | 4 | 4 | unsigned. |
| `ty_ulong` | TY_LONG | 8 | 8 | unsigned. |
| `ty_ulonglong` | TY_LONGLONG | 8 | 8 | unsigned. |
| `ty_float` | TY_FLOAT | 4 | 4 | IEEE 754 single. |
| `ty_double` | TY_DOUBLE | 8 | 8 | IEEE 754 double. |
| `ty_ldouble` | TY_LDOUBLE | 8 | 8 | Apple ARM64: same as double (not 80-bit / 128-bit). |

These globals are referenced by name from many other modules
(`parse.c`, `codegen_arm64.c`, etc.).  The Phase 3 reimplementation
must keep them at the same addresses or break the entire linkage.

---

## Notes for the spec author

Gotchas, non-obvious invariants, and places where a test case would
be valuable:

- **`add_type` is idempotent.** It checks `node->ty` early and bails
  if already set.  This matters because parse.c calls `add_type`
  speculatively in several places, and the type system must not
  recompute or change a type that's already been assigned.

- **`copy_type` sets `origin`, not `next`.** `origin` is the typedef
  back-pointer used by `is_compatible`.  `next` is reset to NULL
  because `copy_type` is for single-Type copies, not for list nodes.
  A future spec must distinguish these clearly.

- **`vector_of` reuses `array_len` for element count.** This is a
  design choice (not a bug), but it means `array_len` is overloaded:
  it's the array length for `TY_ARRAY`, the element count for
  `TY_VECTOR`.  The spec should describe the semantic, not the field.

- **`get_common_type`'s pointer arm fires on `ty1->base &&
  ty1->kind != TY_COMPLEX`.** This assumes a non-complex Type with
  a non-NULL base is a pointer/array/etc.  Vector types ALSO have a
  base, but `vector_of` runs first in the priority order so it's
  never reached.  If the priority order changes, this needs review.

- **Predefined Type globals are statically allocated and shared.**
  Returning `ty_int` (the global address) vs `pointer_to(...)` (a
  fresh malloc) is observably different — the parser/codegen
  identity-compares Type pointers in some places.  The spec must
  preserve this property: never copy `ty_*` globals.

- **`is_compatible` walks `origin` recursively.** A typedef chain of
  N levels does N recursive calls.  In practice corpus chains are
  small; a future profile could replace with iteration if needed.
  Permitted optimization, not required behavior.

- **VLA dereference doesn't decay to pointer in `add_type`.** Per
  the comment at line 386–388, `ND_DEREF` on a VLA returns the VLA
  itself (kind `TY_VLA`); the codegen pipeline handles the decay.
  The spec must describe this asymmetry vs regular array-deref.

- **`__real__` on non-complex is identity.** Documented behavior;
  preserved from chibicc/main.  Test case worth adding to
  `tests/regression/`: `_Static_assert(__real__(3) == 3, ...)`.

- **No I/O, no globals beyond the type singletons.** This makes
  Phase 3 the simplest swap-out so far in terms of state management.
  The implementation is a pure function library.

- **`add_type`'s switch covers ~25 ND_* kinds.** The spec should
  enumerate them by category (arithmetic, comparison, control, var,
  member access, address, deref, statement-expr, GCC builtins) so
  the reimplementer doesn't miss any.  An exhaustive-switch test
  (default with internal-error) is a good defensive pattern.

- **Bitfield promotion in ND_MEMBER is signed/unsigned-aware.**
  Lines 354–365.  Subtle: an unsigned bitfield of width < 32
  promotes to `ty_int` (signed!) per C standard's "value preserving"
  rule, while exactly 32 promotes to `ty_uint`.  Test case worth
  pinning.

- **`ND_DEREF` of a function type returns the function type.**
  Supports GCC `typeof(*func)` semantics.  Test case from real-world
  code: `typeof(*malloc) *fn = ...`.

- **VLA arithmetic decay happens elsewhere.** Lines 386–388 explicitly
  defer VLA-to-pointer decay to `new_add`/`new_sub` (in `parse.c`).
  The spec must capture this division of responsibility — Phase 3
  should not silently move the decay into `add_type`.
