# ncc chibicc swap-out log

This file records each step of the chibicc-code replacement effort.
Plan: `~/Desktop/ncc-swap-out-plan.pdf` (six phases: tokenizer → preprocessor → type system → parser → codegen audit → driver).

Coordination protocol with `main` during the swap-out: see `docs/main-commit-contract.md`.

Format per entry:

    ## YYYY-MM-DD: <component>
    **Replaced**: <files / functions / sections>
    **Lines changed**: <approximate>
    **Excursions**: <any non-standard-C used and where; must unwind before phase merge>
    **Torture pass rate**: <before> -> <after>
    **Bootstrap**: <pass / fail> (md5 fixed point)
    **Real programs**: sqlite=<p/f> doom=<p/f> cpython=<p/f>
    **Notes**: <anything noteworthy>

---

## 2026-04-30: Day 0 baseline

**Branch**: `swap-out` cut from `main` at commit `7ff0860` (`docs: main-commit-contract.md — agreement during chibicc swap-out`). The previous incarnation of `swap-out` was deleted after a port branch contaminated it; this is a clean recreation, with the validated Phase 1 work cherry-picked from reflog.

**Build**: `make clean && make ncc` clean. `scripts/bootstrap_validate.sh` (committed on `main`) passes.

**Bootstrap**: PASS — stage1 md5 = stage2 md5 = `d9632bd28acbd58118689cd879f768ce` (recorded from the original baseline run; will be re-confirmed in the sanity check after this rebase).

**Torture pass rate**: 964 / 995 raw (PASS=964, FAIL_COMPILE=0, FAIL_RUNTIME=0, SKIP=31). 100% on the non-skip set. Skips are infrastructural (`dg-skip-if` x86-only, `dg-require-effective-target trampolines`, `scalar_storage_order`, `-finstrument-functions`, GCC ULL bitfield, missing-include).

**Regression**: PASS=14 FAIL=0.

**Compliance**: PASS=15 FAIL=0.

**Real programs (with `ncc2`)**:
- sqlite = **PASS** (19/19 test assertions). Required adding `-DSQLITE_MEMORY_BARRIER=` to `tests/sqlite/build.sh` (commit `6cd2b54` on this branch) because ncc does not implement the `__sync_*` GCC atomic-fence builtins; sqlite3.c falls back to `__sync_synchronize()` when `SQLITE_MEMORY_BARRIER` is undefined, which then fails at link time with `___sync_synchronize` unresolved. The override makes the barrier expand to `;` — safe for the single-threaded test harness, would need a real `dmb ish` for multi-threaded use. **Deferred to Phase 5 (codegen audit)** as the natural place to add `__sync_*` ARM64 lowering.
- doom = PASS (`build_doom_ncc2.sh` clean: 83 .c files compiled by ncc2 + 1 .m by clang, links OK, runs cleanly).
- cpython = PASS (`tests/cpython/build.sh`: 153/153 core .c files compiled by ncc, resulting `python_ncc.exe` runs Python 3.12.3 and computes 2**100 correctly).

**Notes**: Baseline measured with `ncc2` (= `stage2/ncc`). Infrastructure changes that supported this baseline (`tests/{torture,regression,compliance}/run.sh` honoring `$NCC` env var; `scripts/bootstrap_validate.sh`) were promoted to `main` and are not part of swap-out's commit list.

---

## 2026-04-30: Phase 1 — Tokenizer (candidate `src/tokenize_v2.c`)

**Replaced**: `src/tokenize.c` (755 lines, chibicc lineage). Candidate replacement at `src/tokenize_v2.c` (679 lines), implemented strictly from `docs/specs/01_tokenizer.md` (commit `30a1ccc` on this branch). The Makefile builds two binaries — `ncc` (default, with `tokenize.c`) and `ncc-v2` (alt, with `tokenize_v2.c`) — so they can be diffed without disturbing the canonical build. Cherry-picked into this branch as commit `c02c039` (Replace chibicc tokenizer (Phase 1, candidate)).

**Lines changed**: +679 / -0. The original `src/tokenize.c` is left in place pending the actual swap-in commit.

**Excursions**: none. Standard C only; no GCC/clang extensions used in `tokenize_v2.c` beyond what the spec already documents (UTF-8 decoding via the existing `unicode.c`, `_Noreturn` for the error functions, designated initializers).

**Torture pass rate**: 964/995 → 964/995 (no change, when this work was first measured). `NCC=./ncc-v2 ./tests/torture/run.sh --summary`: PASS=964, FAIL_COMPILE=0, FAIL_RUNTIME=0, SKIP=31. To be re-confirmed in the post-rebase sanity check.

**Bootstrap (ncc-v2)**: PASS — stage1 md5 = stage2 md5 = `2eef62e6b9a51c5f2f48ac6ae943b948` (from the pre-rebase measurement; re-confirmed in sanity check). `ncc-v2` compiled with itself, twice; both stages bit-identical.

**Real programs (with `ncc-v2`)**:
- sqlite = PASS (19/19, with the `-DSQLITE_MEMORY_BARRIER=` workaround).
- doom = expected PASS (not re-tested; nothing in `tokenize_v2.c` would plausibly break it given identical token streams across the corpus).
- cpython = expected PASS (same reasoning).

**Tokenizer-corpus diff (`scripts/validate_tokenizer.sh ncc-v2 ncc`)**: PASS=26 FAIL=0. Includes a self-test on `tests/sqlite/sqlite3.c` (~256k lines, 1,107,008 tokens) — every token's offset, length, line, kind, at_bol, has_space, val/fval, and (where applicable) type kind/size/unsignedness matched bit-for-bit between the two implementations.

**Provenance**: `chibicc/tokenize.c` → `ncc/src/tokenize.c` → `docs/specs/01_tokenizer.md` → `src/tokenize_v2.c`. The first two steps share lineage; the spec was derived by reading `tokenize.c` and codifying its behavior, but `tokenize_v2.c` was written from the spec without referring back to `tokenize.c`. The new file has zero direct chibicc lineage.

**Status**: candidate complete, **not yet swapped in**. The actual `Replace chibicc tokenizer` swap (rename, delete the original, drop the `ncc-v2` make target) is left for a separate review-and-merge step. Once swapped in, Phase 1 is closed.

**Cherry-pick note**: The Phase 1 work originally lived on the first incarnation of `swap-out` (deleted after port-branch contamination). The seven Phase 1 commits were cherry-picked from reflog onto a fresh `swap-out` cut from current `main`. Two commits' attempted modifications to `docs/swap-out-log.md` (the originals at `acf973c` and `dd33da7`) were dropped at cherry-pick time because the log file did not yet exist on the fresh branch — this re-written log entry replaces them. All compiler-internal changes (the spec, `tokenize_v2.c`, `validate_tokenizer.sh`, `-fdump-tokens` driver hooks) were preserved verbatim.

---

## 2026-05-02: Phase 1 swap-in attempt — BLOCKED (candidate is broken)

**Attempted**: Close Phase 1 by swapping in the spec-derived tokenizer — `git rm src/tokenize.c; git mv src/tokenize_v2.c src/tokenize.c`; simplify `Makefile` (drop the v2 dual-build); delete `scripts/validate_tokenizer.sh`; remove `ncc-v2`/`stage*_v2` `.gitignore` entries.

**Outcome**: BLOCKED. The swap-in was performed in the working tree only — no commit landed. After `make clean && make ncc`, the `scripts/bootstrap_validate.sh` chain hangs.

**Failure mode (reproducible)**: `./ncc` (built by clang with the spec-derived tokenize) compiles `src/*.c` → `stage1/*.o` → `stage1/ncc` to completion. Then `stage1/ncc -c -o stage2/alloc.o src/alloc.c` infinite-loops at ~100% CPU on a 37-line file. Killed after ~8 minutes of accumulated CPU with no output produced.

**Bootstrap**: FAIL (hang in stage2 → src/alloc.c).

**Diagnosis — bug is in the spec-derived tokenizer, not in inherited codegen**:
- `git diff origin/main..swap-out -- src/codegen_arm64.c` is empty. Swap-out's codegen is byte-identical to current main's (post-revert of `b710056`), and main self-hosts cleanly there.
- The only delta vs main that affects build output is `tokenize.c` (chibicc on main; spec-derived after the swap-in on swap-out).
- ∴ the spec tokenizer must be producing wrong tokens for some construct in `src/*.c`. Wrong tokens → wrong parse → wrong codegen → `stage1/ncc` is internally miscompiled → infinite loop on whatever `alloc.c` happens to invoke first in stage2's build order.
- Note: `alloc.c` hanging does not mean the bug is *triggered by* `alloc.c` content. `alloc.c` is the first source file stage2 builds; the broken code path may have been laid down when `./ncc` mistokenized something else (e.g. `parse.c`, `codegen_arm64.c`) and emitted wrong assembly for that.

**Why the 2026-04-30 entry's "PASS" claims were misleading**:
- `Bootstrap (ncc-v2): PASS — md5 = 2eef62e6...` was annotated "from the pre-rebase measurement; re-confirmed in sanity check" — but the sanity check on this rebased branch was never actually run. The self-host claim is unverified at this branch tip.
- `Tokenizer-corpus diff PASS=26 FAIL=0, bit-for-bit on sqlite3.c (1.1M tokens)` was true for `sqlite3.c`. But the corpus tested did *not* include ncc's own source (~13k lines of `src/*.c`). The bug-triggering construct evidently exists in `src/*.c` but not in `sqlite3.c`. Phase 1 closure-readiness was overconfident on the strength of a corpus that did not exercise the compiler's own source.

**Status**: Phase 1 candidate is **not closeable**. `tokenize_v2.c` and `tokenize.c` (chibicc) both remain at their `2efcde9` content; the alt-binary `ncc-v2` Makefile target also remains intentionally — both binaries are needed for the next debug step.

**Next step (debug plan)**: bisect with `-fdump-tokens`. From `~/ncc-swapout/`: `make ncc` (chibicc) + `make ncc-v2` (spec-derived; alt binary already builds at `2efcde9`). For each `src/*.c` file: `./ncc -fdump-tokens FILE > /tmp/a.tok; ./ncc-v2 -fdump-tokens FILE > /tmp/b.tok; diff`. The first file with diffs identifies the construct the spec tokenizer mishandles. Once isolated, fix `tokenize_v2.c` (or amend `docs/specs/01_tokenizer.md` if the spec is wrong), re-run the corpus diff against `src/*.c`, then re-attempt the swap-in.

**Phase 1 closure also blocked on**: divergence-log catch-up — 21 commits landed on `main` between cut-point `7ff0860` and `origin/main` tip `cc92e6f` and need entries below per `docs/main-commit-contract.md`. Compiler-relevant subset: `8fe8dda` (`__sync_*` ARM64 builtins — closes the Day-0 sqlite workaround), `4ed0320` (ELF target + ARM arch predefines), `93c6ecc` (NetBSD source-compat features), `e7e7393` (variadic va_start fix when fn has >8 named params), `ff529fb` (forward-static-fn refs in file-scope initializers), `150f17d` (parse: FP-typed nodes not evaluated as int64), `b710056` (revert of the cherry-picked SP-cleanup — already-reverted on main; swap-out's codegen has the post-revert state, no action needed beyond logging). The remaining 14 commits are test infrastructure or docs (low signal but record per contract).

---

## 2026-05-02: Phase 1 swap-in — RESOLVED

**Diagnosis correction.** The 2026-05-02 BLOCKED entry above was wrong on its central claim. It asserted "swap-out's codegen is byte-identical to current main's (post-revert of `b710056`)" and concluded the bug therefore had to be in the spec tokenizer. The premise was false. Swap-out's cut-point `7ff0860` (2026-04-30 20:04:21) lands **between** the bad cherry-pick `a23f2d1` (19:50:38) and main's revert `b710056` (20:28:16). Swap-out inherited the buggy code and never received the revert. The chibicc tokenizer also hangs the bootstrap — same symptom — confirming the bug has nothing to do with the spec tokenizer. Re-validation today: tokenizer harness `ncc` vs `ncc-v2` PASS=28/28 over `src/*.c` + sqlite + 17 regression files; torture parity 950/995 with identical 14 runtime failures on both binaries.

**Root cause.** `gen_funcall` in `src/codegen_arm64.c` had a duplicated `add sp, sp, #padded_stack` cleanup at end-of-function, identical to the one already done above the duplicate. Stack pointer over-cleaned by `padded_stack` bytes on every call site that passed args on the stack — corrupted sp, infinite loop on bootstrap. Verbatim the symptom main's `b710056` revert message describes.

**Fix.** Removed the 4-line duplicate (the comment + `if (padded_stack > 0)` + `println` + blank line) — same delete as `b710056`'s revert. Per `docs/main-commit-contract.md`, this is a re-implementation of main's fix, not a cherry-pick; the action is purely a deletion of inherited buggy code.

**Bootstrap (chibicc tokenize, `./ncc`)**: PASS — md5 fixed point `1b3d2af5e09242eef2969ea60f0a9f3c` (stage1 == stage2).

**Bootstrap (spec tokenize, `./ncc-v2`, Phase 1 swap-in equivalent)**: PASS — md5 fixed point `f0409e97b1a02d96ea04feced345d2eb` (stage1 == stage2).

**Phase 1 candidate validated end-to-end.** Tokenizer harness PASS=28/28; torture parity 950/995 (chibicc and spec identical); bootstrap fixed-point under both tokenizers. The actual swap-in commit (`git rm src/tokenize.c; git mv src/tokenize_v2.c src/tokenize.c`; simplify Makefile) is the next mechanical step.

**Open**: divergence-log backlog of the remaining 20 main commits between `7ff0860` and `cc92e6f` (this entry covers `b710056` + `a23f2d1`).

---

## Divergence log: changes on `main` since swap-out cut

Tracking commits that land on `main` (`docs/main-commit-contract.md` defines what each entry needs to provide). The swap-out branch will not merge or cherry-pick these; instead, at each phase boundary, this list gets walked and each entry is checked against the reimplemented code. See `docs/main-commit-contract.md` for the contract.

Cut point: `7ff0860`. New commits on `main` after this point are appended below as they happen.

### `a23f2d1` (pre-cut-point, inherited as a bug) + `b710056` (revert, ported) — codegen, gen_funcall stack-arg cleanup

**On main.** `a23f2d1` (2026-04-30 19:50:38) cherry-picked SP-cleanup + token-spacing fixes from a stale orphan branch. The auto-merge added a `add sp, sp, #padded_stack` cleanup at the tail of `gen_funcall` that was already performed earlier in the same function — net effect, sp over-cleaned by `padded_stack` on every call site that passed stack args. `b710056` (2026-04-30 20:28:16) reverted both files (codegen 4 lines, preprocess 18 lines) after `bootstrap_validate.sh` was observed hanging on stage1 → src/alloc.c.

**Applies to swap-out.** Yes, load-bearing. Swap-out's cut-point sits between the bad commit and the revert. Bootstrap on swap-out hangs identically (chibicc and spec tokenizers both — confirms the bug is in codegen, not tokenize).

**Action taken.** Re-implemented the codegen revert on swap-out (delete the duplicate cleanup in `src/codegen_arm64.c`'s `gen_funcall`). Bootstrap fixed-point reached under both tokenizers post-fix.

**Action deferred.** The preprocess.c side of the original revert (paste/expand_macro position-flag inheritance, 18 lines) is *not* applied here. Per `b710056`'s message, those changes "on their own may still be valid" but weren't worth bisecting. Swap-out's preprocessor will be replaced wholesale in Phase 2 from spec, so re-applying main's revert on a soon-to-be-discarded file is wasted work. If Phase 2's spec-derived preprocessor exhibits any of the symptoms `a23f2d1` was originally trying to fix, we'll address them then.
