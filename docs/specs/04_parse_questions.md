# Phase 4 Spec — Design Questions

Sixteen numbered design questions distilled from
`04_parse_inventory.md` and the cross-cutting concerns it surfaces.
Each gives context, options, and a recommended default so Wayne can
either accept or push back without re-deriving the analysis.

Phase 4 is materially larger than Phases 1–3: parse.c is ~3.4× the
preprocessor and ~14× the type system.  The questions split roughly
into four buckets:

- **§A. Process / structure** — how to break the spec and the impl
  into reviewable chunks (Q1–Q3).  These need answers first because
  they determine the shape of everything after.
- **§B. Contract preservation** — invariants the rewrite must keep
  (Q4–Q9).
- **§C. Strategy** — scoping calls about what to spec vs what to
  describe-and-defer (Q10–Q13).
- **§D. Tests + closure** — Phase 4 regression tests and the
  swap-in gate (Q14–Q16).

Mostly recommendations are short: parse.c is sprawling but its
externally-observable contract is tight (it produces an AST that
codegen consumes), so the safe default for most questions is
"preserve behavior, document the invariant explicitly."  The
interesting questions are the process ones in §A.

---

## A. Process and structure

### Q1. One spec doc or multiple?

**Context:** Phases 2 and 3 each had a single `0N_xxx.md` spec
(1798 → 1817 lines and 435 → 499 lines respectively).  Parse.c is
6136 lines.  A single Phase 4 spec at the same density would land
around 6–8k lines — large to author in one push and large to
review.

The inventory (§2) identifies seven natural sub-chunks: declaration
specifiers, declarator parsing, constant expression evaluation,
expression grammar, statement parsing, initializer parsing, global
initializer evaluation.  Plus cross-cutting glue (scopes,
typedef/`is_typename`, complex/vector lowering, VLA, attributes).

**Options:**
- **A.** One monolithic `04_parse.md`.  Matches Phases 2–3 rhythm.
  Authoring + review both heavy.
- **B.** Multiple spec files: `04a_decl.md`, `04b_expr.md`,
  `04c_stmt.md`, `04d_init.md`, plus a top-level `04_parse.md` that
  covers cross-cutting concerns and points into the sub-files.
  Cleaner review batches.  More cross-references to maintain.
- **C.** One spec file but author + swap-in incrementally,
  sub-chunk by sub-chunk, with intermediate `parse_v2.c` partials
  that fall back to the canonical impl for un-rewritten zones.

**Recommended:** **B.** Splitting the spec by sub-chunk gives natural
review boundaries (each ~600–1200 lines) and lets §A/B/C/D parallel
the inventory's structure.  **C** sounds attractive but the
fall-back-to-canonical machinery is real engineering with no
durable value — once Phase 4 closes, that scaffolding is dead
weight.  **A** works but the review queue gets unhealthy.

The cross-cutting top-level (`04_parse.md`) is small (~500 lines):
typedef rules, scope mechanics, `is_typename` semantics, AST
contract.  Sub-files cite into it.

### Q2. Spec authoring cadence

**Context:** Phase 2's Q21A was "Wayne batch-reviews after I draft
each major section."  Phase 3 collapsed to "draft end-to-end, single
review pass."  Phase 4 is too big for one review pass and naturally
fits a sub-chunk-per-batch cadence (which is consistent with Q1.B).

**Options:**
- **A.** One sub-chunk drafted, pushed, reviewed, signed off → next
  sub-chunk.  Strict serial.
- **B.** Draft all sub-chunks back-to-back (no waits), review them
  in any order Wayne prefers.  Faster wall clock; risk of late
  feedback rippling backward.
- **C.** Draft top-level + first sub-chunk together so the contract
  surface is settled, then sub-chunks in any order.

**Recommended:** **C.** The cross-cutting top-level is the contract
surface — get it right and the sub-chunks become ~mechanical.
Drafting it alongside one concrete sub-chunk (recommend: declaration
specifiers, the heart of the file) tests whether the cross-cutting
abstractions actually carry weight.

### Q3. Sub-chunked impl + incremental swap-ins, or single big swap-in?

**Context:** Phase 2/3 swap-ins were one-shot: implement `xxx_v2.c`
end-to-end, validate against test corpus, swap in.  Phase 4 candidate
file (`parse_v2.c`) at full scope is ~6k lines.

**Options:**
- **A.** Single one-shot swap-in, matching Phases 2–3.  Big-bang
  validation: bootstrap fixed-point + full test corpus.
- **B.** Sub-chunked swap-ins: `parse_v2.c` initially delegates to
  canonical `parse.c` for everything, sub-chunk-at-a-time the v2
  impl takes over.  Each sub-chunk swap-in is a small validation
  step.
- **C.** Per-sub-chunk swap-in but no scaffolding — Phase 4 produces
  many small commits to the canonical `parse.c` itself, each one
  swapping in a freshly-spec'd zone.  No `parse_v2.c` at all.

**Recommended:** **A.** Same reasoning as Q1: scaffolding for
gradual swap-in is engineering without durable value.  The bootstrap
fixed-point check is a strong gate and the whole test corpus runs
in seconds — big-bang is cheap to validate.  The risk of a bug
hiding in a 6k-line rewrite is real, but the same risk existed at
Phase 2's 1817 lines and was caught fine by the test corpus + fixed
point.  Trust the gate.

If the impl phase reveals this was wrong, fall back to **C**: land
the spec-conformant rewrite zone-by-zone in canonical `parse.c`.
**B**'s scaffolding is the option I'd avoid.

---

## B. Contract preservation

### Q4. AST shape: bit-identical Nodes or semantically-equivalent?

**Context:** Codegen consumes the Node tree parse.c produces.
Bootstrap fixed-point requires that the v2 parser produces a Node
tree such that codegen emits identical bytes.  In principle this
allows trivial differences (e.g., temp-variable names, unused
fields), but in practice the safest contract is "produce the same
Nodes."

**Options:**
- **A.** Spec mandates Node-shape preservation: same node kinds,
  same children, same types, same line-info, same generated names.
  Bootstrap fixed-point is the gate.
- **B.** Spec allows semantically-equivalent Nodes (same observable
  codegen output) but not necessarily bit-identical AST.  Tested
  via end-to-end output, not AST inspection.

**Recommended:** **A.** Bootstrap fixed-point is the cheapest and
sharpest validator we have; it requires byte-identical compiler
output, which in practice means identical-enough AST.  **B** sounds
flexible but it's flexibility we don't need, and it makes
divergence diagnosis harder ("which Node differs?" → "I dunno,
output's wrong").

The spec should explicitly note where temp-variable names are
allowed to differ (the only known case: `new_anon_gvar`'s counter is
process-wide so v1 and v2 numbering may diverge — but in self-host
they run separately, so each pass is internally consistent).

### Q5. `is_typename` exhaustive enumeration

**Context:** `is_typename(tok)` (§2.6, §3.1 of inventory) is the
watershed function: every parse decision of the form "is this a type
or an expression?" flows through it.  The current impl is a static
hashmap of keywords plus a typedef-scope lookup.  A misclassified
token corrupts everything downstream.

**Options:**
- **A.** Spec enumerates the full token set verbatim (~40 keywords +
  typedef-name lookup).  Reimpl matches keyword-for-keyword.
- **B.** Spec describes the categories ("storage class keywords",
  "qualifiers", "primitive type keywords", "GCC extension type
  keywords", "typedef-name lookup") and cites C11 §6.7 + named GCC
  extensions for the inclusion criteria.

**Recommended:** **A.** This is exactly the kind of place where "the
list IS the spec."  Categories are useful prose framing but the
enumeration is normative.  The spec should have a single canonical
list that the reimpl mirrors as a table-driven check.

### Q6. `eval_node` parse-time fold scope

**Context:** parse.c has its own constant folder (`eval_node`,
`try_eval_node`, ~460 lines including the complex evaluator).
Codegen has a separate folder.  Inventory §3.2 calls out the
two-mode (fatal / falsey) design as load-bearing.

**Options:**
- **A.** Preserve the existing fold scope exactly.  Spec enumerates
  the operators and types it folds (same as today).
- **B.** Spec defines a minimum required fold scope (anything used
  by array-dim, bitfield-width, enum-value, static-init, sizeof,
  `_Static_assert`) and lets the reimpl fold more aggressively if
  it wants.

**Recommended:** **A.** Folds-more-aggressively sounds harmless until
codegen-fold and parse-fold disagree on a corner case (e.g.,
overflow).  Contract is "fold exactly what's needed for the
parse-time use cases; defer everything else to codegen."  Spec
enumerates explicitly.

### Q7. Complex / vector parse-time lowering

**Context:** Inventory §3.3, §3.4: `_Complex T` and
`__attribute__((vector_size(N)))` arithmetic is decomposed into
scalar real/imag and element-wise ops at parse time.  Codegen never
sees a "complex multiply" or "vector add" — it sees a sequence of
scalar ops on a tmp variable.

**Options:**
- **A.** Preserve.  Spec describes the lowering pattern explicitly
  with worked examples for each operator.
- **B.** Defer to codegen (introduce `ND_COMPLEX_MUL` etc.).  Out of
  Phase 4 scope, but worth noting if Phase 5 wants it.

**Recommended:** **A.**  This is the established contract.  Phase 5
might revisit (real SIMD codegen would benefit from raising the
abstraction) but Phase 4's job is preservation.

### Q8. VLA hidden-local insertion point

**Context:** Inventory §3.5: a VLA decl `int a[f()]` produces a
hidden local `__vla_size_N` plus a runtime size-computation
statement.  The statement must be inserted into the surrounding
block *before* the declaration's first use.  This is not in the C
standard; it's a chibicc convention.

**Options:**
- **A.** Spec explicitly defines "before the declaration's first
  use" with a worked example (declaration mid-block → size-compute
  inserted at declaration point, not at block top).
- **B.** Spec leaves insertion point implementation-defined.

**Recommended:** **A.**  This is exactly the kind of invariant where
"trust the implementation" produces subtle bugs years later.  Worked
example pays for itself.

### Q9. Bootstrap fixed-point as the contract

**Context:** parse.c → codegen interface is the AST.  AST is too
detailed to test directly.  Self-host bootstrap fixed-point (ncc
compiles itself, the resulting binary compiles itself, both
binaries are byte-identical) is a strong and cheap proxy.

**Options:**
- **A.** Phase 4 closure gate is `scripts/bootstrap_validate.sh`
  exit 0 + full test corpus pass.  Same gate as Phase 2/3.
- **B.** Add an AST-dumping mode to ncc and diff the AST between v1
  and v2 directly.  More precise but materially more engineering.

**Recommended:** **A.**  Same gate that closed Phase 2 and Phase 3.
**B** is interesting infrastructure that we don't need yet — if a
divergence is discovered post-Phase-4 that fixed-point misses, we
build it then.

---

## C. Strategy

### Q10. Nested functions (GCC extension) — preserve parse path?

**Context:** Inventory §3.7: nested function syntax is parsed and
codegen attempts trampolines, which fail at runtime on macOS due to
W^X.  Most torture tests are SKIP'd via `dg-require-effective-
target trampolines`.  The parser does work; the runtime is broken.

**Options:**
- **A.** Preserve parse-side support fully.  Spec describes nested-
  function semantics so the rewrite produces the same captured-
  variable Nodes.  Runtime brokenness is Phase 5's concern.
- **B.** Drop parse-side support.  Reject nested functions with a
  clear error.  Saves ~50 lines.  Breaks any source code that uses
  them, even though the runtime is already broken.

**Recommended:** **A.**  Symmetric "same parser shape, same codegen
shape" rule applies even when the codegen path is broken.  When
Phase 5 fixes runtime (or chooses not to), the parser already
produces the right thing.  Cost: ~50 lines of preserved logic.
Benefit: no surprise regression for anyone using the GCC extension
on Linux.

### Q11. Designated initializer brace-elision

**Context:** Inventory §3.8 + last-bullet "gotchas": designated init
+ brace-elision + partial overrides combined are subtle.  C11 §6.7.9
specifies the rules but chibicc's implementation has been corpus-
tested over years.

**Options:**
- **A.** Spec describes by reference to C11 §6.7.9 + cites chibicc
  as the canonical implementation.  Includes 2–3 worked examples
  for the trickiest cases (partial override, brace-elided union
  init).
- **B.** Spec inlines the full algorithm (effectively a port of
  chibicc's init.c logic into prose).
- **C.** Spec describes the algorithm at high level, defers to test
  corpus for ground truth.

**Recommended:** **A.**  Standard citation + worked examples is the
sweet spot.  Pure prose translation (B) duplicates code and rots.
Test-as-spec (C) is fine for in-scope corner cases but bad for the
"what is the contract?" question.

### Q12. `__attribute__` parsed-and-dispatched vs parsed-and-ignored

**Context:** Inventory §3.10 + last-bullet gotchas: parse.c accepts
many attributes but only some have codegen meaning.  E.g.,
`packed`, `aligned`, `section`, `weak`, `used` are honored;
`cleanup` is parsed but not lowered; many GCC-specific ones are
silently ignored.

**Options:**
- **A.** Spec enumerates each attribute as one of: HONORED (lowered
  by codegen), PARSED-AND-IGNORED (accepted, no effect), REJECTED
  (error).  Reimpl mirrors.
- **B.** Spec lists only HONORED attributes; everything else is
  PARSED-AND-IGNORED by default.  Simpler but loses the explicit
  REJECTED set.

**Recommended:** **A.**  The explicit table is short (~15
attributes) and removes a class of "does ncc support X?"
questions.  Worth the half-page.

### Q13. Forward-declared static-fn fix (`ff529fb`) — port during Phase 4?

**Context:** Inventory's last bullet flags `ff529fb` as a known
main-branch fix gated on Phase 4.  It's a parser-level fix:
forward-declared static fns referenced in file-scope initializers
were previously rejected.

**Options:**
- **A.** Spec includes the fixed behavior; reimpl matches main
  post-`ff529fb`.  A new compliance test (`tests/compliance/`)
  lands alongside.
- **B.** Spec describes pre-fix behavior; port `ff529fb` after Phase
  4 closes via a separate divergence-log walk.

**Recommended:** **A.**  Phase 4 is the natural moment.  Porting
later means going back into the same code with full context-load
overhead — just do it now.  The compliance test pins the fix
durably.

(User said "skip divergence backlog" today; Q13 is the lone
divergence-backlog item that genuinely belongs *inside* Phase 4
because the fix is a parser fix.  Treating it as Phase 4 work, not
backlog work.  The other three gated items — `150f17d`, `e7e7393`,
`93c6ecc` — stay deferred per user direction.)

---

## D. Tests + closure

### Q14. Phase 4 regression tests

**Context:** Test corpus has ~25 regression tests today (Phase 1–3
contract tests included).  Phase 4 introduces no new contracts in
the AST sense (it's a rewrite, not a feature), but it's a good
moment to pin invariants the spec calls out as load-bearing.

**Options:** (additions, not exclusive)
- **A.** `NN_typedef_shadow.c` — Q5 / typedef + ordinary namespace
  separation: a typedef `T` and a variable `T` coexist correctly.
- **B.** `NN_designated_init_partial.c` — Q11 / partial override +
  brace elision combined.
- **C.** `NN_vla_side_effect.c` — Q8 / VLA size expression with
  observable side effect (e.g., `int a[printf("hi") ? 5 : 5]`).
- **D.** `NN_static_fn_forward.c` — Q13 / `ff529fb` repro: forward-
  declared static fn used in file-scope initializer.
- **E.** `NN_complex_mul_assoc.c` — Q7 / complex multiplication
  associativity: `(a+bi) * (c+di) * (e+fi)` matches expected
  decomposed form.
- **F.** `NN_attr_table.c` — Q12 / probe each HONORED attribute
  produces observable codegen change; each PARSED-AND-IGNORED is
  silently accepted.

**Recommended:** **B, C, D, F** at minimum.  **A** is well-covered
by the existing torture; **E** is nice-to-have but the existing
complex tests already exercise the lowering — not high-priority.

### Q15. Cross-phase coupling — Phase 5 (codegen) interactions

**Context:** Phase 4 and Phase 5 share the AST contract.  Several
inventory items punt to Phase 5: real SIMD (Q7 alternative),
trampoline runtime (Q10), labels-as-values codegen, `__atomic_*`
lowering, variadic-fix `e7e7393`.

**Options:**
- **A.** Phase 4 spec is silent on Phase 5 work.  Each phase's spec
  is self-contained.
- **B.** Phase 4 spec includes a §13 ("Phase 5 prerequisites") that
  enumerates AST-level invariants Phase 5 will rely on.

**Recommended:** **B.**  Phase 5 is the next phase; bootstrapping
its inventory will need exactly this list.  Cheap to write now,
expensive to reconstruct later.

### Q16. Phase 4 closure tag policy

**Context:** Phases 1/2/3 each ended with `phase-N-closed` annotated
tag (per `feedback_tag_phase_boundaries`).  Phase 4's size means a
single closure moment may be uncomfortably far away if (Q3) we
big-bang the swap-in.

**Options:**
- **A.** Single `phase-4-closed` tag at full swap-in + bootstrap
  fixed-point + full corpus pass.  Same as Phases 1–3.
- **B.** Intermediate tags per sub-chunk (`phase-4a-closed`,
  `phase-4b-closed`, ...) plus final `phase-4-closed`.  Useful only
  if (Q3) we go incremental.

**Recommended:** **A.**  Conditional on Q3.A.  If Q3 flips to
incremental, Q16 should follow.

---

## E. Recommendation summary

| Q | Topic | Recommendation |
|---|---|---|
| Q1 | Spec doc structure | **B** — multiple sub-chunk files |
| Q2 | Authoring cadence | **C** — top-level + first sub-chunk together, then any-order |
| Q3 | Impl/swap-in strategy | **A** — single big-bang swap-in |
| Q4 | AST shape contract | **A** — Node-shape preservation, fixed-point as gate |
| Q5 | `is_typename` enumeration | **A** — exhaustive list in spec |
| Q6 | `eval_node` fold scope | **A** — preserve exactly |
| Q7 | Complex/vector lowering | **A** — preserve parse-time decomposition |
| Q8 | VLA insertion point | **A** — define explicitly with worked example |
| Q9 | Closure gate | **A** — bootstrap fixed-point + corpus |
| Q10 | Nested fns | **A** — preserve parse path |
| Q11 | Designated init brace elision | **A** — C11 cite + worked examples |
| Q12 | Attribute disposition | **A** — explicit HONORED/IGNORED/REJECTED table |
| Q13 | `ff529fb` port | **A** — port during Phase 4 |
| Q14 | Regression tests | **B, C, D, F** |
| Q15 | Phase 5 prereqs | **B** — enumerate at end of spec |
| Q16 | Closure tag policy | **A** — single tag (matches Q3.A) |

The big decisions are **Q1, Q2, Q3** — they shape everything that
follows.  Once those are settled, the rest is mostly preserve-
existing-contract.

---

## F. Resolutions (2026-05-04)

All sixteen questions resolved by Wayne in interactive Q&A.

| Q | Resolution | Match recommendation? |
|---|---|---|
| Q1 | **B** — multi-file spec (top-level + 04a/04b/04c/04d sub-chunks) | yes |
| Q2 | **C** — top-level + first sub-chunk drafted together, then any-order | yes |
| Q3 | **A** — single big-bang swap-in via parse_v2.c | yes |
| Q4 | **A** — Node-shape preservation; bootstrap fixed-point as gate | yes |
| Q5 | **A** — exhaustive is_typename keyword enumeration in spec | yes |
| Q6 | **A** — eval_node fold scope preserved exactly | yes |
| Q7 | **A** — complex/vector parse-time lowering preserved | yes |
| Q8 | **A** — VLA hidden-local insertion point defined explicitly | yes |
| Q9 | **A** — bootstrap fixed-point + full corpus as closure gate | yes |
| Q10 | **A** — nested fn parse path preserved (runtime is Phase 5) | yes |
| Q11 | **A** — C11 §6.7.9 cite + 2–3 worked examples | yes |
| Q12 | **A** — explicit HONORED / PARSED-AND-IGNORED / REJECTED table | yes |
| Q13 | **A** — port ff529fb fix during Phase 4 (parser fix in parser phase) | yes |
| Q14 | **B, C, D, F** — four regression tests (revised 2026-05-05) | yes |
| Q15 | **B** — Phase 5 prereqs section at end of spec | yes |
| Q16 | **A** — single phase-4-closed tag (consistent with Q3.A) | yes |

All recommendations accepted as proposed.  (Q14 was initially
expanded to all six tests on 2026-05-04 and revised back to the
original four-test recommendation on 2026-05-05 after the spec
draft surfaced that A is well-covered by torture and E is
redundant with the existing complex-lowering tests.)

These resolutions are the source of truth for spec authoring.
Subsequent docs (`04_parse.md`, `04a_decl.md`, etc.) cite question
numbers when invoking a baked-in decision.
