# `main` commit contract (during swap-out)

While the chibicc swap-out is in progress on the `swap-out` branch, work on `main` (linux-port, netbsd-port, bug fixes, infrastructure) continues independently. This document defines what `main` commits need to convey so the swap-out branch can stay correct without merging from `main`.

## How the two streams stay aligned

The swap-out branch **does not merge or cherry-pick** from `main` for compiler-internal files. Instead, at each phase boundary, the swap-out work walks new `main` commits and asks, for each one: *does the same bug or feature need to exist in the reimplemented code?* If yes, it's fixed on `swap-out` via spec + reimplementation, not by porting the patch.

This preserves the audit claim that swap-out's compiler-internal code is derived from English specifications, not transplanted from existing source.

## Scope

The contract applies to commits that modify any of these "compiler-internal" files:

```
src/tokenize.c
src/preprocess.c
src/type.c
src/parse.c
src/codegen_arm64.c
src/main.c
src/cc.h
include/builtins.c
```

Commits that touch only non-internal files (`tests/`, `scripts/linux_scan/`, `Makefile`, `README.md`, `CLAUDE.md`, headers in `include/` other than `builtins.c`, `docs/`) are unaffected — those flow into `swap-out` freely by absorption when convenient.

## What each compiler-internal commit must provide

### 1. Functional subject line

The subject says **what behavior changed**, in what subsystem, in roughly what bug class — not just which file was edited.

Good:
- `parse: fold compound-literal of pointer type to its initializer value`
- `tokenize: error_tok / warn_tok report at macro use site, not definition`
- `codegen_arm64: stage large sub/add sp immediates through scratch register`

Bad:
- `parse: fix bug`
- `update parse.c`
- `cleanup`

A reader (human or otherwise) should be able to decide, from the subject alone, whether their own implementation is plausibly affected.

### 2. Body with enough context to recognize the bug

Two to five sentences. What was broken, what input or condition triggered it, what the fix does conceptually. The body is for *recognition* — readers don't need the patch reproduced, they need to know whether their code has the same blind spot.

### 3. Regression test in `tests/regression/` (for bug fixes — gold standard)

A minimal `.c` repro committed alongside the fix, named `NN_short_description.c` matching the existing convention (e.g. `tests/regression/01_incomplete_struct_array_init.c`).

This is the highest-value artifact. The swap-out branch can run the repro against its reimplemented compiler and the verdict is mechanical:
- **pass** → the new code already handles this case correctly; mark covered
- **fail** → the bug exists in the new code; fix and add the same regression test

`tests/regression/` is non-internal, so it flows into `swap-out` automatically — no reimplementation needed for the test itself, only for the underlying fix if the test fails.

### 4. New features get an exercising test

For new flags, new builtins, new behaviors: add a test under `tests/compliance/` or `tests/regression/` that exercises the feature. swap-out picks up the test and the new behavior becomes a checkable item against the reimplemented compiler.

## What happens if (1)–(3) are missing

The swap-out walk has to read the diff to infer functional intent, which is exactly what this contract is meant to avoid. The cleaner the commit messages, the cheaper the divergence-log walk at each phase boundary, and the lower the risk that a real bug fix on `main` gets silently missed in swap-out.

## When the swap-out finishes

Once the swap-out completes (all six phases shipped, `swap-out` merges into `main`), this contract is no longer needed. Until then, treat it as the working agreement between `main` and `swap-out`.
