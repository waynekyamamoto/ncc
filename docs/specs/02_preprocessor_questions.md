# Phase 2 Spec — Design Questions

This document collects the design decisions that need answers before
`docs/specs/02_preprocessor.md` can be written end-to-end. Each question
distills a non-obvious choice surfaced by `02_preprocessor_inventory.md`
or by a cross-cutting concern (bug-vs-feature, stub-vs-implement,
strictness, driver interface).

For each question, the format is:

> **Context:** what's there today, why it's a question.
> **Options:** A / B / C with implications.
> **Recommended:** my best guess at the right answer + the reasoning, so
> you can either accept the default or push back. **Recommended doesn't
> mean decided** — your sign-off (or redirect) on each is what closes it.

The spec drafted in §4–§12 of `02_preprocessor.md` will reference these
question numbers so the lineage of each decision is explicit.

---

## A. Bugs surfaced by the inventory

### Q1. Deduplicate `__SCHAR_MAX__` in the new `init_macros`?

**Context:** `init_macros` calls `define_macro("__SCHAR_MAX__", "127")`
twice (current `src/preprocess.c:1245` and `:1296`). Same value both
times, so `hashmap_put` silently keeps the second; no observable
behavioral difference today. But it's a latent bug — if either call
ever drifted to a different value, the result would depend on call
order.

**Options:**
- **A.** Spec lists `__SCHAR_MAX__` exactly once. Reimpl defines it
  once. (Clean; behaviorally identical.)
- **B.** Spec lists it twice to faithfully document main, with a note
  flagging the duplicate as a quirk.

**Recommended:** **A.** This is the kind of cleanup the swap-out is
*for*. The spec should describe the deduplicated set; if a future
divergence-log walk catches main also deduping, great, otherwise we've
silently improved.

---

### Q2. Fix `fopen` leak in `search_include_paths`?

**Context:** Current `src/preprocess.c:244` does `if (fopen(path, "r"))`
without closing the returned `FILE *`. One leaked FD per include-path
probe per `#include`. Hits the open-file-descriptor limit on huge
projects (Linux kernel: 1000s of include probes per TU).

**Options:**
- **A.** Spec uses `access(path, R_OK)` (POSIX, but ubiquitous) for
  existence probing. Reimpl follows.
- **B.** Spec uses `stat(path, ...)` + check `errno` (more portable on
  paper but same POSIX dependency). Reimpl follows.
- **C.** Preserve the leak to match main bit-for-bit; flag as a known
  quirk in the spec's §13 (divergences).

**Recommended:** **A** (`access`). Same pure-C constraint as the rest
of the project — `access` is POSIX, but so is `open_memstream` /
`dirname` / `strdup`, all of which Q14 already commits us to replacing
or relying on. `access` is cheaper than `fopen`+`fclose` and doesn't
leak. The fix is invisible to any test (FD count isn't asserted), but
it's the right behavior.

---

### Q3. Should the new `hideset_union` deduplicate?

**Context:** Current implementation appends without dedup
(`src/preprocess.c:117`). Inventory notes "duplicates are harmless
because `hideset_contains` terminates on first match" but they
accumulate on deeply-nested expansion. Memory cost scales with
nesting depth × call frequency.

**Options:**
- **A.** Preserve no-dedup behavior. Simpler code; existing torture
  passes; no observable behavior change.
- **B.** Dedup on union. Slightly slower per union (linear scan),
  faster per `contains` (smaller hideset), bounded memory.
- **C.** Use a different data structure (sorted list, hash set) for
  better asymptotic bounds.

**Recommended:** **A.** Behavioral compatibility is the project's
actual measurable property; algorithmic improvements are scope creep
unless we have a real perf complaint. The spec should describe the
linked-list semantics and note that dedup is a permitted optimization
if a future profile shows it matters.

---

## B. Stubs — implement, keep as stub, or document explicitly?

### Q4. `#pragma once` — implement or keep stub?

**Context:** Current behavior: recognized but no-op
(`src/preprocess.c:1758-1761`). Re-`#include`ing a file with `#pragma
once` includes it again. Real-world impact: most modern headers use
include guards instead, so the bug rarely surfaces; but some Apple SDK
headers and many third-party libs rely on `#pragma once`.

**Options:**
- **A.** Keep as stub. Spec documents it as a known simplification;
  add a regression test that exercises both `#pragma once` and
  `#ifndef`-guard, asserting the guard works.
- **B.** Implement: track included canonical paths in a set; on
  `#pragma once`, mark the current file; on `#include`, skip if
  marked. Maybe ~20 lines.
- **C.** Implement, but only check by `realpath`-resolved canonical
  path (handles symlinks, multiple `-I` paths to the same file).

**Recommended:** **A** for Phase 2. Keep behavioral compat with main.
But: **add a regression test** under `tests/regression/NN_pragma_once.c`
that documents the divergence (or absence of one) so a future move to
**B** has a target. If real-program builds (sqlite/cpython) start
failing on it, switch to **C**.

---

### Q5. `__has_feature` always returns 0 — keep or extend?

**Context:** `src/preprocess.c:408-409` returns 0 for every input. This
forces clang-feature-gated headers down the C-path (usually correct
for C99/C11 code).

**Options:**
- **A.** Keep returning 0. Spec documents it.
- **B.** Return 1 for a small allowlist of features ncc actually
  supports (`__has_feature(c_atomic)` → 0 honestly,
  `__has_feature(modules)` → 0, etc.). Most useful entries: maybe
  `c_alignas`, `c_alignof`, `c_static_assert`.
- **C.** Mirror clang's full table for the macOS SDK we target.

**Recommended:** **A.** **B/C** are slippery slopes — clang's `__has_feature`
list grows constantly. Returning 0 is a stable, predictable contract
that says "we're a minimal C11 compiler, take the standard path." If
a header takes a wrong branch as a result, that's the header's bug
under the standard contract.

---

### Q6. `__TIMESTAMP__` returns "Unknown" — implement or document?

**Context:** `timestamp_macro` (`src/preprocess.c:1185`) always returns
`"Unknown"`. A real impl would stat the source file and format
mtime. Almost no real C code uses `__TIMESTAMP__` for anything other
than build banners.

**Options:**
- **A.** Keep stub. Document in §13.
- **B.** Implement: `stat(file)` + `ctime` formatting. ~10 lines.

**Recommended:** **A.** Useless feature, stub is clearer than a
half-working implementation. Document the divergence.

---

## C. Subtle invariants that must be explicit in the spec

### Q7. Function-like-macro detection rule (`!has_space && '('`)

**Context:** `read_macro_definition` at `:1546` distinguishes
`#define FOO(x) ...` (function-like) from `#define FOO (x)`
(object-like with body `(x)`) using `!tok->has_space && equal(tok,
"(")`. The spec must describe this rule precisely; tokenizer
correctness here is load-bearing for preprocessor correctness.

**Options:**
- **A.** Spec states the rule verbatim and the spec for §3 (token
  field usage) requires `has_space` to be set per-token by the
  tokenizer.
- **B.** Spec uses the C11 wording ("a left parenthesis with no
  intervening white space") and requires the tokenizer to set
  `has_space` such that the chibicc-style check works.

**Recommended:** **B.** Use C11 standard wording for the rule;
implementation-detail (`has_space` field) goes in the data-model
section. Cleaner spec, same outcome.

---

### Q8. `subst` rescan-vs-stringize asymmetry

**Context:** Argument substitution uses `arg->expanded` (pre-rescanned);
stringization uses `arg->tok` (raw, not rescanned). This is C11's
"rescan before substitution but not before stringization" rule. Easy
to get wrong; the inventory flags it as a place a test would be
valuable.

**Options:**
- **A.** Spec dedicates a subsection to this rule with the C11
  citation and a worked example. Add a regression test
  (`tests/regression/NN_stringize_no_rescan.c`).
- **B.** Spec mentions the rule in passing alongside §2.1.

**Recommended:** **A.** This is the single most subtle preprocessor
rule in the C standard; a worked example pays for itself.

---

### Q9. Hideset asymmetry (object-like uses union; function-like uses intersection)

**Context:** Object-like macro body's hideset = `union(tok->hideset,
{m->name})`. Function-like body's hideset =
`union(intersection(macro_tok->hideset, rparen->hideset), {m->name})`.
The intersection is the C standard's "painter's rule" for function-like.

**Options:**
- **A.** Spec describes both with the standard citation; explicit
  worked examples for the painter's rule.
- **B.** Spec gives just the rule without citation/examples.

**Recommended:** **A.** Same reasoning as Q8 — high-subtlety rule,
worth the page count.

---

### Q10. `origin` set on body only, not on appended tail

**Context:** `expand_macro` walks the body up to `TK_EOF` and sets
`origin = tok` on each body token; the appended `tok->next` tail is
not touched. This is correct (`error_tok`'s origin walk lands on the
expansion site only for tokens that came from the macro body), but
non-obvious.

**Options:**
- **A.** Spec describes the rule and gives the rationale (`error_tok`
  semantics).
- **B.** Spec just describes the operation; rationale lives in code
  comments.

**Recommended:** **A.** The spec is supposed to be the source of
truth; rationale in the spec lets a future reimplementer not
accidentally "fix" it.

---

### Q11. `include_file` mutates the included file's `TK_EOF`

**Context:** `:1492-1493` overwrites the included file's terminal
`TK_EOF` node's fields with the continuing token, in place. Saves
allocation; but means callers must not hold a reference to the
original `TK_EOF` (the kind changes).

**Options:**
- **A.** Spec describes the splice as "logically equivalent to
  appending; implementation may choose to mutate the EOF sentinel for
  efficiency" — leaves room for a non-mutating reimpl.
- **B.** Spec mandates the in-place mutation pattern (matches main
  exactly).

**Recommended:** **A.** Underspecify the mechanism, specify the
behavior. A future allocator change might want a non-mutating splice;
the spec shouldn't foreclose that.

---

### Q12. `line_delta` propagation rules for tokens inside macro expansions

**Context:** `preprocess2`'s main loop writes `tok->line_delta =
tok->file->line_delta` on every output token *that passes through
that branch*. Tokens emitted from macro body expansion do not pass
through that branch — they retain whatever `line_delta` they had from
`copy_token` of the body. Inventory flags this as a place to clarify.

**Options:**
- **A.** Spec defines `line_delta` as "the value of `file->line_delta`
  at the moment the token was *first* emitted by the preprocessor."
  For body tokens, that's the `#define` site's `line_delta`.
- **B.** Spec defines `line_delta` as "the value of `file->line_delta`
  at the macro-call site, propagated to all expanded tokens." Would
  require a code change.
- **C.** Spec acknowledges this is an inconsistency and documents
  current behavior under §13 divergences.

**Recommended:** **C** (then maybe **A** later). The current behavior
is what main does; documenting it as a known inconsistency is honest.
Don't change semantics in Phase 2.

---

## D. Allowlists / hardcoded tables

### Q13. `__has_attribute` / `__has_builtin` allowlists

**Context:** Hardcoded lists at `:354-364` and `:379-394`. They go
stale every time someone adds a builtin or attribute support to the
parser/codegen.

**Options:**
- **A.** Keep hardcoded list in `read_const_expr`, document each
  allowlist in the spec verbatim. Maintenance: any new attribute or
  builtin requires a spec update too.
- **B.** Move the list to `cc.h` (a single `static const char *`
  array) shared between preprocess and parse; spec defines the
  invariant ("the list must be kept in sync with parse.c's
  recognition").
- **C.** Compute the list dynamically by querying the parser/codegen
  module (would require new API surface).

**Recommended:** **B**. Single source of truth, doesn't change Phase
2's scope much (the list is still a C array literal, just at module
scope). Spec describes the contract; concrete list lives in `cc.h`.

---

## E. Standards strictness

### Q14. POSIX-ism replacement strategy (`open_memstream`, `dirname`, `strdup`)

**Context:** Per pure-C-implementation rule
(`feedback_pure_c_implementation.md`), the new `preprocess.c` must
use only C11. Current uses: `open_memstream` (3 sites), `dirname`
(2 sites), `strdup`/`strndup` (throughout).

**Options:**
- **A.** Replace each with a C11 helper inlined in `preprocess.c`.
  Fast, contained, but duplicates work that other modules also need.
- **B.** Add C11-replacement helpers to `cc.h`/`alloc.c`/a new
  `compat.c`, used across the preprocessor and (later) other phases'
  reimpls.
- **C.** Drop pure-C constraint; keep POSIX deps, document in spec
  that the implementation depends on POSIX.

**Recommended:** **B.** A small `compat.c` (or extend `alloc.c`) with
`mem_buf_*`, `path_dirname`, `mem_strdup`, `mem_strndup` becomes a
shared utility for the rest of the swap-out phases. Spec describes
the helpers as part of the data model.

---

### Q15. `__STDC_NO_ATOMICS__` = 1 vs the `__atomic_*` stubs

**Context:** ncc advertises `__STDC_NO_ATOMICS__` = 1 (we don't
support C11 `_Atomic`). But it also defines `__atomic_load_n` etc.
as single-threaded stubs (GCC-compat builtins, not C11 `_Atomic`).
Headers can gate on either: `#ifndef __STDC_NO_ATOMICS__` (sees us
as no-atomics, takes fallback path) or `#ifdef
__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8` (sees us as having atomics, takes
the GCC path).

**Options:**
- **A.** Keep current contradictory advertisement; document in spec
  as deliberate (gives header authors more options).
- **B.** Drop the GCC `__sync_*` claim — only `__STDC_NO_ATOMICS__` =
  1 stays. Headers consistently take the no-atomics path. Risk:
  some headers may break.
- **C.** Drop `__STDC_NO_ATOMICS__` = 1 — claim full atomics support.
  Risk: parser doesn't actually understand `_Atomic`, will fail on
  any code that hits it.

**Recommended:** **A.** Don't fix a contradiction that real programs
work around successfully. sqlite/doom/cpython all build today; the
contradiction is load-bearing in some way we don't fully understand.
Document it honestly in §13 ("ncc claims both no-atomics and
GCC-style sync builtins; this is intentional for header-compat").

---

## F. Driver / module interface

### Q16. Module state — globals or context struct?

**Context:** Current `preprocess.c` has globals: `include_paths`
(`StringArray`), `cond_incl` (linked stack), `pragma_handler`
(callback), `counter_val`, `base_file`. The spec needs to describe
how the driver (`main.c`) and the preprocessor share state.

**Options:**
- **A.** Spec describes module-level globals (matches current code).
  Phase 6 (driver swap-out) can refactor if desired.
- **B.** Spec describes a `Preprocessor` context struct passed to
  every entry point. Cleaner architecturally; foreshadows Phase 6
  cleanly. Bigger churn for Phase 2.
- **C.** Compromise: spec describes the *logical* interface
  (init/preprocess/teardown) and lets the implementation choose
  globals or context. Phase 6 can reify whichever isn't there.

**Recommended:** **C.** Spec describes the logical operations; the
implementation choice is left open. Phase 2 reimpl uses globals
(matches main, minimal scope). Phase 6 can refactor toward a context
struct if desirable. The spec doesn't have to commit either way.

---

## G. Test coverage

### Q17. What new regression tests should accompany Phase 2?

**Context:** `tests/regression/` has 16 numbered cases today, several
already preprocessor-related (e.g., `11_preprocessor_predefines.c`).
Several inventory items recommend adding tests.

**Options:** (these are all additions, not exclusive)
- **A.** `NN_stringize_no_rescan.c` — Q8 worked example.
- **B.** `NN_painters_rule.c` — Q9 function-like hideset asymmetry.
- **C.** `NN_pragma_once.c` — Q4 documented behavior (whichever way
  decided).
- **D.** `NN_paste_empty_arg.c` — empty-arg `##` skip in `subst`.
- **E.** `NN_directive_in_arg.c` — `handle_pp_directive_in_arg`
  Linux-style pattern.
- **F.** `NN_zero_object_macro.c` — `#define ZERO 0` then `#if ZERO`.
- **G.** `NN_include_next.c` — `#include_next` chain.
- **H.** `NN_va_opt.c` — `__VA_OPT__` empty/non-empty paths.

**Recommended:** **A, B, D, F, H** at minimum (the subtlest semantics
+ ones the spec relies on). **C, E, G** are useful but can be added
post-Phase-2. **Order:** add these with the spec, before the reimpl,
so they exercise the current preprocessor and define the contract;
the reimpl then has to pass them.

---

## H. Future-proofing

### Q18. `-target elf` predefines — where do they slot when Phase 5 brings them back?

**Context:** Phase 2 spec scopes to macOS only. Phase 5 will
reintroduce `-target elf` predefines (`__ELF__`, `__ARM_ARCH`,
`__ARM_ARCH_8A__`, `__ARM_PCS_AAPCS64`). Spec needs to leave a clean
hook for this.

**Options:**
- **A.** Spec describes a single `init_macros()` that sets the macOS
  set. Phase 5 adds an `init_macros_elf()` plus a target-dispatch
  selector.
- **B.** Spec describes `init_macros(target)` from the start, with
  `target = TARGET_MACOS` as the only currently-implemented value.
  Phase 5 adds `TARGET_ELF`.
- **C.** Spec only describes the macOS set; Phase 5 figures out the
  refactoring then.

**Recommended:** **B.** Pure-thoughts mode: design the foundation for
the foreseeable extension (target-dispatch) without building it.
Adding the parameter now costs ~5 lines and makes Phase 5 a one-line
dispatch instead of a refactor.

---

### Q19. Should the spec describe `-E` output formatting?

**Context:** `ncc -E` produces preprocessed C. clang's `-E`
emits `# N "filename" flags` line markers; ncc currently doesn't.
This affects whether `validate_preprocessor.sh` can compare
ncc-vs-clang directly (currently can't without normalization).

**Options:**
- **A.** Spec describes the current minimal format (no line markers).
  Add line markers in a separate phase if needed.
- **B.** Spec describes GCC-style line markers (`# N "filename" 1 2 3
  4`). Reimpl emits them. Lets `validate_preprocessor.sh` compare
  against clang directly.
- **C.** Spec describes line markers as an optional output mode
  (`-fpreprocessed -E` or similar flag).

**Recommended:** **A.** Phase 2 scope; line-marker emission is
genuinely orthogonal to preprocessor correctness. The harness already
handles ncc-vs-ncc comparison; ncc-vs-clang requires SDK-header
alignment too, which is out of scope.

---

### Q20. Inventory's "Notes for spec author" — anything missing?

**Context:** The inventory called out 17 items in its closing notes.
This questions doc covers most as Q1–Q15. Anything I didn't elevate
to a question?

**Items I treated as "describe in spec, no decision needed":**
- `find_macro` accepts both `TK_IDENT` and `TK_KEYWORD` (just describe).
- `at_bol` not modified by preprocessor (just describe).
- `hideset`/`origin` rely on tokenizer-NULL initial values (data-model
  invariant, describe in §3).
- `__DATE__`/`__TIME__` fixed at `init_macros()` time (describe + §13
  divergence).

If you want any of these elevated to an explicit decision question,
flag in your review.

---

## Process question

### Q21. Spec authoring cadence?

**Context:** We discussed earlier (this conversation) that the
synchronous co-authoring model is slow. Two alternative cadences:

**Options:**
- **A.** I draft each section, push, you batch-review every N
  sections and redirect.
- **B.** I draft the full spec end-to-end, push, you review the whole
  thing once.
- **C.** Resume synchronous co-authoring section-by-section (status
  quo).

**Recommended:** **A**, with N=3 sections at a time. Catches direction
errors early without fragmenting your attention; matches your stated
preference for not being the bottleneck on every line.
