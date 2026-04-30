# ncc code-improver — plan and spec

A spec for building a self-improving C compiler around `ncc`: its agentic
improvement loop, its measurement harness, and the clean-room rewrite that
may eventually carry it.

This document is the first read for any continuation of this work.

---

## Status (as of 2026-04-29)

- Compiler: `ncc` — single-pass C11 compiler targeting ARM64 / macOS (Apple
  Silicon).
- Self-hosting: `stage1 == stage2` byte-identical (md5
  `98a10e09a55854d9f4cd60431db32506`, 816,688 bytes at the time of writing).
- GCC torture: **964/995 attempted pass with 0 failures**; 31 intentional
  skips (x87 / MMX / SSE; nested-fn trampolines under macOS W^X;
  `scalar_storage_order`; `-finstrument-functions`).
- Real apps known to compile and run: SQLite, DOOM (doomgeneric),
  CPython 3.12.3.

## Provenance — important

ncc's frontend (~10K LOC: `tokenize.c`, `preprocess.c`, `parse.c`, `type.c`,
`cc.h`, `hashmap.c`, `unicode.c`) is a fork of
[chibicc](https://github.com/rui314/chibicc) by Rui Ueyama (MIT license).
Chibicc fingerprints in the source: identical file layout, identical Token
struct, identical TokenKind enum, identical
`consume(Token **rest, Token *tok, ...)` parser idiom, identical Hideset
macro-expansion machinery.

The ARM64 code generator (`codegen_arm64.c`, ~2.8K LOC) is original — chibicc
targets x86_64; the AArch64 backend was written for this project. The arena
allocator (`alloc.c`) and various extensions (NLGoto, additional GCC
builtins, more attributes, `.file` directives, ELF output mode) are also
original.

This plan covers both the existing chibicc-derived ncc and a future
clean-room reimplementation that may replace it. Most deliverables (harness,
perf metrics, pattern catalog, regression suite) are valuable to either.

## Goals

Four tracks that share a single piece of infrastructure (the test harness):

1. **Codegen quality** — make ncc emit faster, smaller ARM64 code.
2. **Codegen correctness** — surface and fix bugs (especially codegen) along
   the way.
3. **Late peephole optimizer** on emitted ARM64 asm — a pass between codegen
   output and the assembler.
4. **Agentic loop** — semi-autonomous: the agent proposes a targeted compiler
   change → harness measures → keep wins, revert losses → loop.

## Architectural decisions

- **Track 3 = (b): peephole on emitted ARM64 asm.** Not (a) source-level
  rewriting of user C programs. Folds into the codegen track; not a separate
  phase.
- **IR refactor is deferred.** ncc is currently single-pass AST→ARM64.
  Introducing an IR is a large cross-cutting refactor on a self-hosting
  compiler — too risky as a prerequisite. Instead, bias Phase 1 work toward
  late peephole + bug fixes + documented patterns so ≥80% of value transfers
  if/when IR is added later.
- **Harness first.** No compiler edits before measurement is trustworthy.
- **Clean-room rewrite (when undertaken) introduces an IR as deliberate
  structural divergence from chibicc.** Serves both technical goals (better
  optimization potential) and intellectual-ownership goals (a structurally
  different architecture).

### IR reconsideration triggers

- Phase 1 plateaus, AND
- rejected proposals cluster on optimizations that fundamentally need basic
  blocks / SSA / real register allocation (CSE, GVN, LICM, graph-coloring
  regalloc).

## Metrics

### Correctness (gating — must hold every iteration)

- All previously-attempted torture tests pass with same output.
- Self-host fixed point: `md5(stage1/ncc) == md5(stage2/ncc)`.
- Real-app smoke: ncc compiles itself, SQLite, DOOM, CPython subset.

### Performance (reward signal)

- **Static** (primary, deterministic, noise-free):
  - per-function instruction count
  - per-function bytes
  - instruction-mix breakdown (loads / stores / branches / muls / divs)
  - parsed from ncc-emitted asm.
- **Dynamic** (secondary, noisy):
  - p50 / p99 wall time over ~100 in-process iterations.
  - Single-shot timing on tiny programs is dominated by macOS process
    startup (3–5 ms) — only averaged measurements are signal.
- **Aggregate**: weighted sum across the test corpus.
- **Improvement** = score down on ≥3 tests, regression on 0.

Static metrics are primary because they're noise-free and the agent can
reason about them directly. Dynamic numbers corroborate.

## Phase plan

### Phase 0 — Harness (1–2 days, no compiler changes)

Deliverables:

- `bench/run.sh` — runs torture + real apps; emits per-test JSON
  `{name, compile, run, asm_bytes, instrs, instr_mix, ns_p50, ns_p99}`.
- `bench/instcount.py` (~100 LOC) — parses emitted asm, counts by class.
- `bench/snapshot.sh` — produces `baseline-<sha>.json`.
- `bench/compare.py` — diffs two snapshots; prints improvement / regression
  table.
- `bench/full_check.sh` —
  `make clean → clang-build → stage1 → stage2 → fixed-point → torture →
  real apps`. Single command, exit 0 = green.
- `make bench` target wiring it together.

Acceptance: produce `baseline-<sha>.json` for the current commit; review
before any compiler change.

### Phase 1 — Easy codegen wins + peephole framework (~1 week first batch, ongoing)

- New file `src/peephole.c` — late pass over emitted asm before write-out.
  Starts with no rules; pure scaffolding.
- Peephole rules, one per commit, each harness-gated. Candidates:
  - `mov xN, xN` elimination (no-op move)
  - load immediately after store of same address/reg → drop the load
  - `mov xN, #a; add xN, xN, #b` → single `mov xN, #(a+b)`
  - dead-store elimination across straight-line code
  - branch-to-next elimination
- 2–3 in-`codegen_arm64.c` edits identified by reading the file: better
  immediate selection for add/sub, fewer spills in expression eval. Each its
  own commit.
- **Pattern catalog** (`docs/patterns.md`): one entry per win — pattern,
  current emission, improved emission, rationale. **This is the IR-survival
  document.**

### Phase 2 — Agentic loop (2–3 days wiring, then it runs)

- `loop/propose.md` — prompt template. Inputs: baseline + recent diffs +
  harness output. Output: unified diff + rationale + which pattern it
  targets.
- `loop/driver.sh` — apply patch → `bench/full_check.sh` →
  `bench/compare.py` → if improved & green, commit + roll baseline forward;
  else discard. Hard wallclock timeout per iteration.
- Constraints: only `src/*.c` and `src/peephole.c` editable; ≤300 LOC per
  patch; auto-pause after 5 consecutive no-progress iterations.
- Runner: supervised via `/loop` first; promote to `/schedule` for unattended
  runs after ~10 iterations of trust-building.

### Phase 3 — IR (only on trigger conditions)

- Small linear IR (SSA-lite, basic blocks, virtual registers) between parse
  and codegen.
- Keep current codegen as fallback during transition; same harness gates the
  migration.
- Unlocks: real register allocation, CSE, DCE beyond AST level, LICM.

### Phase 4 — Folded into Phase 1

Track 3 (b) is the peephole pass scaffolded in Phase 1. Not a separate phase.

## Bug discovery as loop output

Track 2 (find compiler bugs) is a free output of the loop:

- Every rejected agent proposal is a near-miss → log the failing test →
  human review.
- Periodic differential test: compile sample programs with ncc and clang,
  compare outputs.
- Csmith integration if coverage proves insufficient (deferred).

## Salvage from current ncc (relevant to a clean-room rewrite)

If a clean-room rewrite is undertaken, the following **fully survive**:

| Artifact | Survives | Notes |
|---|---|---|
| `tests/torture/` (GCC test corpus) | yes | GCC's; not chibicc's |
| `tests/regression/{01..14}_*.c` | yes | Real C language pitfalls; portable |
| `tests/compliance/`, `sqlite/`, `doom/`, `cpython/` | yes | Yours |
| `Makefile`, `build_doom_ncc2.sh`, bootstrap_validate procedure | yes | Yours |
| `include/` (macOS SDK shims) | yes | Apple/POSIX surface |
| `scripts/linux_scan/{linux_fix.h, scan.sh, skip_list.txt, stubs/}` | yes | CONFIG defines + scan infra; not compiler internals |
| `linux_port_report.md` (1051-line strategic doc) | yes | Pure analysis |
| xv6-aarch64 build plan (in `CLAUDE.md`) | yes | Compiler-agnostic |
| ARM64 codegen patterns / lessons | knowledge yes / code no | Chibicc-bound code does not transfer |
| Bug catalog (the 14 regression repros) | yes | Each repro is a portable C file |
| ABI lessons (struct return, packed padding, extern inline) | yes | Documented in `linux_port_report.md` |

The following must be **rewritten**:

| Artifact | Why |
|---|---|
| `src/tokenize.c`, `preprocess.c`, `parse.c`, `type.c`, `cc.h`, `hashmap.c`, `unicode.c` | Chibicc-derived; cannot be safely refactored to "from scratch" status |
| `src/codegen_arm64.c` | Original code, but written against chibicc's `Node`/`Type`/`Obj` API. **Pattern knowledge transfers; code does not.** |
| `src/alloc.c` | Trivial (37 LOC); rewrite from spec |

## Clean-room methodology (for the rewrite project)

If you start a clean-room project to claim independent authorship:

**Sources of truth (read these instead of chibicc):**

- C11 standard (ISO/IEC 9899:2011; draft N1570 is freely available)
- ARM Architecture Procedure Call Standard for AArch64 (AAPCS64)
- Mach-O File Format Reference (Apple)
- macOS dyld ABI documentation

**Stop reading chibicc source.** You may read other compilers (TCC, lcc, the
LLVM tutorial) for inspiration on specific problems, but not for
data-structure or organization templates.

**Make at least one deliberate structural divergence.** Recommended:
introduce a real linear IR (SSA-lite, basic blocks, virtual registers).
Chibicc is single-pass AST → asm; yours becomes
parse → IR → optimize → emit. This single choice is enough to make the
codebase structurally yours and earns its keep technically.

### Sequencing (rough estimates)

| Step | Description | Estimate |
|---|---|---|
| A | Harness — build Phase 0 against current ncc; reuse to gate the new compiler | ~1 week |
| B | Minimum viable compiler — lex → parse → emit ARM64 for `int main() { return N; }` | 2–4 weeks |
| C | Grow language — types, expressions, control flow, functions, pointers, arrays, structs, enums, typedefs, bitfields, varargs | 8–16 weeks |
| D | Preprocessor — object/function macros, conditionals, hideset, `#line` | 3–6 weeks |
| E | IR + codegen separation — design the boundary in early; don't bolt on at the end | 2–4 weeks design + ongoing |
| F | Self-host — fixed point achieved | 4–8 weeks of bug-chasing |
| G | Pass parity — reach existing torture pass rate, then beat it | open-ended |

**Don't dual-maintain.** Pick one path:

1. **Rewrite track only**: archive current ncc; new repo or `ncc-v2/`
   directory; single focus.
2. **Improvement track only**: drop rewrite ambition, add MIT attribution
   to chibicc, ship the agentic loop on the existing code.

You cannot realistically run both simultaneously: the improvement loop needs
a stable codebase; the rewrite means the codebase is in flux.

## Honest framing for the rewrite project

**Defensible claims after a clean-room rewrite:**

- "C compiler I wrote from scratch, targeting ARM64/macOS, self-hosting."
- "Designed against the C11 spec and AAPCS64 with my own IR."
- "X thousand lines of C, X% of the GCC torture suite passing."

**Avoid:**

- "Never looked at any other compiler" — can't prove; probably not true.
- "No prior art consulted" — specs are prior art, and that is fine.

## Open questions to resolve before code lands

1. **Production target set**: SQLite + DOOM + self-compile sufficient as the
   real-app perf benchmark? Or add Lua, CPython subset, etc.?
2. **Loop autonomy**: human-review each commit for the first ~10 iterations,
   then auto-commit on green? Or autonomous from iteration 1?
3. **Loop runner**: local `/loop` initially, `/schedule` once trusted? Or
   jump to `/schedule`?
4. **Reward signal**: confirm "static instr count + 100-iter wall p50" as
   the metric. Cycle counters on Apple Silicon need entitlements / `kperf`
   setup — only worth it if static stops discriminating.
5. **Differential testing**: enable from the start vs. defer until a
   regression slips through?

## Artifact location index

Where things live at the time this doc was written. Several items are
present **only** on the author's machine and need to be migrated into a
durable, version-controlled location before a clean-room rewrite begins.

| Artifact | Location | Status |
|---|---|---|
| This document | `PLAN.md` (this file) | new in this commit |
| Live compiler source | legacy ncc working tree (≈13 commits ahead of `origin/main`) | mostly unpushed |
| GitHub mirror | `github.com/waynekyamamoto/ncc` | stale relative to legacy |
| `linux_port_report.{md,html,docx}` | gitignored in legacy ncc; copy at `~/ncc-linux-port/` | **not in any git history** |
| `scripts/linux_scan/*` (canonical) | legacy ncc | in repo, unpushed |
| `tests/regression/{01..14}_*.c` | legacy ncc | in repo, unpushed |
| Salvage staging | `~/ncc-linux-port/` | local-only, partially stale |

**Before starting the clean-room project, the gitignored
`linux_port_report.md` should be moved into version control somewhere
durable** — either the new project's own repo or by being added to this
repo. Recommended: this repo, since it's the spec repo for the work, not
the implementation repo.

## License note

This document discloses that ncc's frontend is derived from chibicc
(MIT, © Rui Ueyama). The repository should include the chibicc LICENSE
text and an acknowledgment in the README. As of this writing that
attribution is missing — fixing it is independent of the rewrite decision
and should happen regardless.
