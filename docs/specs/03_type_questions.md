# Phase 3 Spec — Design Questions

Twelve numbered design questions distilled from
`03_type_inventory.md` and cross-cutting concerns.  Each gives
context, options, and a recommended default so Wayne can either
accept or push back without re-deriving the analysis.

Phase 3 is materially smaller than Phase 2 (435 vs 1798 lines, no
POSIX deps, no GCC extensions in source), so most questions are
short.

---

## A. Singletons and identity

### Q1. Predefined Type singleton identity

**Context:** `ty_void`, `ty_int`, etc. are file-scope compound
literals at fixed addresses.  parse.c and codegen_arm64.c
identity-compare Type pointers in places (e.g., `ty == ty_int`),
not just `ty->kind == TY_INT`.  If Phase 3 reallocated these (e.g.,
returned a fresh malloc), some comparisons would silently break.

**Options:**
- **A.** Spec preserves the file-scope-compound-literal pattern for
  singletons.  Reimpl uses the same address-of-literal trick.
- **B.** Spec switches to function calls (e.g., `Type *get_ty_int(void)`)
  and updates parse.c / codegen_arm64.c identity-compares to use
  `kind`.  More refactoring, no behavior change.

**Recommended:** **A.** Singletons are a stable contract; changing
the API ripples into modules outside Phase 3 scope.  Spec
documents the invariant explicitly.

---

## B. Implementation strategy

### Q2. `is_compatible` recursion vs iteration

**Context:** Current `is_compatible` recurses through `origin`
chain (typedef transparency) and through `base` (pointer/array/etc).
A typedef chain of N levels does N recursive calls.

**Options:**
- **A.** Preserve recursion (simpler, matches main).
- **B.** Convert to iteration (more code, no observable improvement
  on the corpus).

**Recommended:** **A.** Recursive is clearer and the typedef chains
in real corpus are bounded (~5 levels max).  If a profile ever
shows this hot, optimize then.

### Q3. `copy_type`'s origin-only-not-next semantics

**Context:** `copy_type(ty)` returns a new Type with all fields
copied, then sets `ret->origin = ty` and `ret->next = NULL`.  The
`origin` set is for typedef back-tracking (used by `is_compatible`
to walk through typedef layers).  The `next = NULL` reset is so
the caller doesn't accidentally inherit a list link.

**Options:**
- **A.** Preserve both.  Spec documents that copy_type is for
  single-Type copies, not list nodes.

**Recommended:** **A.** Only one path — preserve.  Spec must call
out the `next = NULL` reset explicitly so a future reader doesn't
"helpfully" preserve `next`.

### Q4. `vector_of`'s `array_len` field reuse

**Context:** `vector_of(base, total_size)` stores the element count
(`total_size / base->size`) in `Type->array_len`.  This overloads
the field — for `TY_ARRAY` it's the array length, for `TY_VECTOR`
it's the element count.  Different semantics, same field.

**Options:**
- **A.** Preserve the overload.  Spec describes
  `array_len`-when-vector as "element count" semantically.
- **B.** Add a new `vector_elem_count` field to struct Type.  Cleaner
  but ripples cc.h.

**Recommended:** **A.** No real cost to the overload; spec just
describes the semantic.

---

## C. Subtle invariants the spec must capture

### Q5. `add_type` idempotency

**Context:** `add_type(node)` checks `node->ty` early and bails if
already set.  parse.c calls add_type speculatively in places.

**Options:**
- **A.** Spec mandates idempotency.  Tests would add a regression
  catching a non-idempotent reimpl.

**Recommended:** **A.** Add a regression test (Q11.A below).

### Q6. VLA decay deferral

**Context:** `add_type` on `ND_DEREF` of a VLA leaves the type as
VLA (does not decay to pointer).  Decay happens in `new_add` /
`new_sub` (in parse.c) when VLA is used in pointer arithmetic.
This is a division of responsibility between type.c and parse.c.

**Options:**
- **A.** Preserve.  Spec calls out the asymmetry explicitly: type.c
  does NOT decay VLA; parse.c does, in arithmetic context.

**Recommended:** **A.** Important to document.  Phase 4 (parser)
should re-confirm.

### Q7. Bitfield promotion sign rules

**Context:** `ND_MEMBER` on a bitfield: an unsigned bitfield of
width < 32 promotes to `ty_int` (signed!) per C standard's
"value preserving" rule; width == 32 promotes to `ty_uint`; signed
bitfield of width <= 32 promotes to `ty_int`.

**Options:**
- **A.** Preserve exactly.  Spec describes the rule with worked
  examples.

**Recommended:** **A.** Subtle rule — worked example pays for itself
in the spec, and a regression test pins it (Q11.B).

### Q8. `__real__` / `__imag__` on non-complex

**Context:** GCC convention: `__real__(x)` on a non-complex `x` is
identity (returns `x`); `__imag__(x)` is zero (typed as `x`'s type).
Type system doesn't distinguish — both return `node->lhs->ty`.

**Options:**
- **A.** Preserve.  Spec documents the GCC convention with
  citation.

**Recommended:** **A.** Already in the inventory's gotchas.

### Q9. `ND_DEREF` of function type

**Context:** Per GCC `typeof` extension, `*(func)` has the function
type (not the function's return type).  Used by patterns like
`typeof(*malloc) *fn = ...`.

**Options:**
- **A.** Preserve.  Spec documents the GCC extension by reference.

**Recommended:** **A.**

### Q10. `long double` size on Apple ARM64

**Context:** `ty_ldouble` size = 8 (matches Apple ARM64 ABI).  On
x86_64 it would be 16; on PowerPC it would be 12.  This is a
target-ABI fact, not a free choice.

**Options:**
- **A.** Hard-code size 8 in the predefined globals; document the
  ABI dependency.
- **B.** Parameterize on target (preview of multi-target work).

**Recommended:** **A** for Phase 3 scope.  Phase 5 (codegen audit)
is the natural place to introduce target parameterization.

---

## D. Test coverage

### Q11. New regression tests for Phase 3

**Context:** Currently 23 regression tests; none specifically pin
Phase 3 type-system invariants.

**Options:** (additions, not exclusive)
- **A.** `NN_add_type_idempotent.c` — Q5 invariant.  Compile a
  function and verify it does not error from re-typing.  Hard to
  trigger from C source; may need a parser-level test instead.
- **B.** `NN_bitfield_promotion.c` — Q7 sign rules.  Mix of widths
  + signed/unsigned + verify type via `_Generic` or sizeof.
- **C.** `NN_real_imag_nonscalar.c` — Q8: `__real__(3) == 3`.
- **D.** `NN_typeof_func.c` — Q9: `typeof(*malloc) *fn = malloc;`.
- **E.** `NN_typedef_compatible.c` — typedef chain compatibility.

**Recommended:** **B, C, D, E** at minimum.  Skip **A** (idempotency
is hard to test from C source — pin via spec wording + code review
instead).

---

## E. Process

### Q12. Cadence

**Context:** Phase 2 used Q21A: I draft sections, push, you batch-
review.  That worked.  Phase 3 is materially smaller — could fit
in fewer batches.

**Options:**
- **A.** End-to-end draft, push, you review the whole thing once
  (matches the smaller scope).
- **B.** Two batches (data model + invariants vs add_type +
  validation).
- **C.** Synchronous co-authoring.

**Recommended:** **A** for Phase 3 specifically.  type.c is small
and tightly-bound; splitting the spec into batches doesn't aid
review the way it did for the preprocessor's sprawling §4–§12.
