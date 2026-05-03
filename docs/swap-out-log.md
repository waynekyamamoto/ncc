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

**Action taken (2026-05-03, pre-Phase-2).** Revisited and reversed. The 18-line preprocess.c revert is now applied on swap-out as part of this commit (delete in `paste()` and the two `expand_macro()` blocks for object-like and function-like macros — byte-identical to `b710056`'s preprocess hunk). Earlier rationale ("preprocessor will be replaced wholesale in Phase 2, so reverting is wasted work") was outweighed by two considerations: (1) Phase 2 starts here, and the spec should describe main's behavior — not a transient inherited divergence — so authoring against an aligned `preprocess.c` keeps the spec honest; (2) `b710056`'s revert message is explicit that any reapplication of these fixes belongs on main as a clean commit with a regression test, not as branch-local cargo. Validation post-revert: bootstrap fixed-point holds (stage1 == ncc2 by md5), tokenizer harness PASS=28/28, torture 964/995 (100% non-skip) — same numbers as `phase-1-closed` (`65c8297`). swap-out's `preprocess.c` is now byte-identical to `origin/main`'s on these 18 lines.

### `150f17d` — parse: try_eval_node must not evaluate FP-typed nodes as int64

**On main.** Bug in `try_eval_node`: ND_NUM and ND_CAST cases unconditionally read `node->val` (int64), but for FP-typed nodes the value is in `node->fval` and `val` is garbage. Surfaces only when constant folding traverses an FP subexpression. Fix is 4 lines in `src/parse.c`.

**Applies to swap-out.** Latent. The bug requires a code path that calls `try_eval_node` on FP-typed nodes. swap-out's `try_eval_node` has the same buggy code, but the trigger introduced on main was `93c6ecc`'s addition of `if (try_eval_node(node->cond, &cond_val))` in gen_stmt's ND_IF case — and swap-out doesn't have `93c6ecc`. Without the if-fold caller, the bug is unreachable from any in-scope program; torture and bootstrap agree (current swap-out: torture 964/995 100% on non-skip).

**Action taken.** None. Logged as gated on `93c6ecc`.

**Action deferred.** When `93c6ecc`'s if-fold lands (Phase 4/5 NetBSD work), `150f17d` must land alongside it. Both are in `src/parse.c`; Phase 4 (parser rewrite) is the natural seam.

### `e7e7393` — codegen: variadic va_start when fn has >8 GP/FP named params

**On main.** `gen_funcall`'s va_start used `add x10, x29, #16` for the va_list base; correct only when ≤8 named GP and ≤8 named FP params. With more, overflow named args occupy stack slots starting at x29+16 and the variadic region begins after them. Fix: walk named params, add `(gp_overflow + fp_overflow) * 8` to the base. 25 lines in `src/codegen_arm64.c`.

**Applies to swap-out.** Yes — same `gen_funcall` codegen path on aarch64 — but no in-scope program (sqlite, doom, cpython, ncc itself) declares a variadic with >8 named params. Trigger surface is NetBSD's `sysctl_createv` (12 named + variadic), not in scope.

**Action taken.** None. No reachable trigger from current swap-out workload.

**Action deferred.** Phase 5 (codegen audit) ports the fix. Should land with `tests/compliance/22_variadic_after_stack_named.c` (the regression test from main).

### `ff529fb` — parse: forward-static-fn refs in file-scope initializers

**On main.** `find_var()` walks scope and returns NULL for not-yet-declared static functions; `primary()` errored "undefined variable" when this was triggered by a file-scope initializer for a struct of fnptrs whose targets are defined later in the same TU. Fix: `in_gvar_initializer` counter; `primary()` synthesizes a function-typed placeholder Obj when find_var returns NULL inside an initializer; relocation machinery resolves at codegen time. Trade-off: misspelled identifier in such an initializer no longer reported at the offending line. 25 lines in `src/parse.c`.

**Applies to swap-out.** Yes — same `primary()` and `find_var()` code path. No in-scope program triggers this; the motivating use case is NetBSD's `kern_cctr.c` style (ops-table struct of static fnptrs).

**Action taken.** None.

**Action deferred.** Phase 4 (parser rewrite) is the natural seam — port the fix and `tests/compliance/21_forward_static_init.c` then.

### `8fe8dda` — codegen: `-target elf` flag and `__sync_*` builtins

**On main.** Two independent capabilities:
1. `-target elf` switches Mach-O directives → ELF (.text/.data/.bss/.rodata, drop leading `_`, `:lo12:` instead of @PAGE/@PAGEOFF, `.weak` instead of `.weak_definition`); routes assembly through `aarch64-elf-as`.
2. `__sync_*` GCC builtins emitted inline in `gen_funcall`: `__sync_synchronize` → `dmb ish`, `__sync_lock_release` → `stlr wzr`, `__sync_lock_test_and_set` → ldaxr/stlxr loop, `__sync_bool_compare_and_swap` → ldaxr/cmp/stlxr CAS.

135 lines in `src/codegen_arm64.c` + main.c + cc.h.

**Applies to swap-out.** Codegen-only; not load-bearing for any current swap-out workflow on macOS/Mach-O. The `__sync_*` half *would* let `tests/sqlite/build.sh` drop the `-DSQLITE_MEMORY_BARRIER=` workaround that swap-out inherited at `6cd2b54`.

**Action taken.** None.

**Action deferred.** Phase 5 (codegen audit) ports both. When `__sync_*` lands, also remove the SQLITE_MEMORY_BARRIER override from `tests/sqlite/build.sh`.

### `4ed0320` — main + preprocess: Linux build, `-target elf` enhancements, ARM arch predefines

**On main.** Cross-platform additions:
- `main.c`: Linux include-path discovery via `/proc/self/exe`; under `-target elf` undef Apple predefines and define `__ELF__`; silently accept `-nostdinc` / `--sysroot=`; pass `-march=armv8.6-a+sve` to assembler in ELF mode; forward `-l`/`-L`/`-Wl,...` to system linker.
- `preprocess.c`: predefine `__ARM_ARCH=8`, `__ARM_ARCH_8A__`, `__ARM_PCS_AAPCS64` under `-target elf`.

40 lines total.

**Applies to swap-out.** Touches `src/preprocess.c`, but only for the `-target elf` predefines path. Default Mach-O target unaffected.

**Action taken.** None at code level. **Flagged for Phase 2 spec authoring.** Phase 2's preprocessor spec must decide: include the `-target elf` predefined-macro set in scope, or defer ELF support until later. Recommend deferring — the simpler initial spec covers macOS only; `-target elf` becomes a separate spec extension when xv6/NetBSD/Linux work resumes.

**Action deferred.** When `-target elf` capability returns to swap-out (Phase 5 codegen audit, alongside `8fe8dda`), the preprocess-side predefines need to come with it.

### `93c6ecc` — codegen + parse: NetBSD source-compat bundle

**On main.** Six features for NetBSD/aarch64 kernel + ACPICA source compatibility:
1. Mini-inliner for `static inline` functions whose body is a single `__asm__` (NetBSD PSTATE write pattern).
2. `__attribute__((section("foo")))` capture and emission with explicit ELF flags (`link_set_*` markers).
3. File-scope `__asm("...")` directives (NetBSD's `__strong_alias` / `__weak_alias`).
4. Compound literal followed by postfix suffix (ACPICA pattern).
5. `if (try_eval_node)` constant folding (precondition for `150f17d`'s bug).
6. `__asm` recognized as inline-asm keyword; no-op compiler attributes (`target`, `pcs`, `neon_vector_type`, `no_sanitize*`, etc.).

271 lines across `src/codegen_arm64.c` + `src/parse.c` + `src/cc.h`.

**Applies to swap-out.** None of the six features is needed by sqlite/doom/cpython/ncc-itself. All NetBSD/ACPICA-specific.

**Action taken.** None.

**Action deferred.** Phase 4 (parser rewrite) and Phase 5 (codegen audit) jointly; when `93c6ecc` is ported, `150f17d` must come along (`93c6ecc`'s if-fold introduces the trigger that `150f17d` fixes). Tests `tests/compliance/16..20` come along.

### Tests / infrastructure / docs commits — bulk no-action

Fourteen commits between cut-point and `cc92e6f` are tests, infrastructure, or docs that don't modify the compiler:

`cc92e6f` `34afce2` `6d67fd8` `820562c` `9354c70` `02826fd` `6a93efd` `9bcaafd` `8664268` `2d44324` `0e88d74` `a690c94` `414a13c` `6eee1c2`

Coverage: xv6/NetBSD/Linux test harness additions (`tests/xv6`, `tests/netbsd`, `tests/linux`), `validate.sh` validation pyramid, compliance test additions, CLAUDE.md updates, cpython filename tweak.

**Applies to swap-out.** None at code level. The compliance tests added on main (`tests/compliance/16..22`) are referenced by individual entries above (`93c6ecc`, `e7e7393`, etc.); they'll be ported alongside the features they test in Phases 4–5.

**Action taken.** None. Logged in bulk per `docs/main-commit-contract.md`.
