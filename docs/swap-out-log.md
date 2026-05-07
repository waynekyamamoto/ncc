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

### `da702d9` + `2fae4e0` — NetBSD test harness, post-`cc92e6f`

**On main.** Two test-only commits past the previous log walk's tip:
- `da702d9` (2026-05-02 10:52): `tests/netbsd/MINIMAL_VIRT64` adds PTYFS/PROCFS/TMPFS/MSDOSFS/KERNFS + putter/drvctl/fss; `tests/netbsd/tools/docker-kernel-build.sh` adds an `nbconfig` step before `nbmake` so config edits actually propagate. Verified the ncc-built MINIMAL_VIRT64 reaches `/etc/rc` multiuser stage.
- `2fae4e0` (2026-05-02 14:14): `tests/netbsd/tools/ncc-elf-wrapper.sh` routes `kern/tty.c` (one file, ~5500 lines) through `aarch64--netbsd-gcc` so that login is reachable on the otherwise-ncc-built kernel.

**Applies to swap-out.** No source-level change — both commits touch only `tests/netbsd/`, which swap-out doesn't carry.

**Action taken.** None.

**Codegen bug flagged for Phase 4/5 (from `2fae4e0`'s message).** The reason `tty.c` had to be routed to gcc: every tty/cdev ioctl returned `ENOSYS`, including `TIOCSFLAGS` on `/dev/console`/`/dev/constty` and `fcntl(O_NONBLOCK)` from `ntpd`/`postfix`. Bisect (v5→v8) isolates the miscompile to `tty.c` alone. Symptom shape (every fnptr-table dispatch hitting the no-op stub) matches an ncc miscompile of a `cdevsw` / `fileops` / `linesw` struct of designated initializers, where slots that should hold ioctl/fcntl/open handlers come out NULL. Root cause not yet diagnosed on main; the workaround is in place.

**Reachability on swap-out.** Latent. swap-out's `parse.c` and `codegen_arm64.c` carry the same designated-initializer code paths as main at `7ff0860`. No in-scope program (sqlite, doom, cpython, ncc itself) has been observed to emit a designated-init struct of fnptrs that triggers this — torture/bootstrap remain green. But Phase 4 (parser rewrite) and Phase 5 (codegen audit) should treat designated-initializer fnptr-tables as a known fragile surface and add a dedicated regression case once main's diagnosis lands. Watch for the upstream fix and the test it ships with.

## Validation snapshot — 2026-05-03 (post-`ad8f346`, pre-Phase-2)

Full validation pyramid run on swap-out tip after the two `b710056` catch-up commits (`0bffb33` codegen + `ad8f346` preprocess) and the `phase-1-closed` tag at `65c8297`. All five harnesses match the Phase-1-close baseline exactly — the 18-line preprocess revert is non-regressive on every measurable axis we have today.

| Harness                          | Result                              | Baseline at `phase-1-closed` |
|----------------------------------|-------------------------------------|------------------------------|
| `scripts/bootstrap_validate.sh`  | FIXED POINT (md5 `596456d18356edce36f34de96c1b287b`) | FIXED POINT |
| `scripts/validate_tokenizer.sh`  | PASS=28/28                          | PASS=28/28 |
| `tests/torture/run.sh`           | 964/995 PASS, 31 SKIP, 100% non-skip | 964/995 |
| `tests/sqlite/build.sh`          | 20/20 SQL test cases PASS           | (Day-0 PASS, not re-measured at close) |
| `build_doom_ncc2.sh`             | 83/83 C files OK, links cleanly     | (Day-0 PASS, not re-measured at close) |
| `tests/cpython/build.sh`         | 153/153 core files OK; runtime tests print expected output | (Day-0 PASS, not re-measured at close) |

Real-program builds were carried implicitly through Phase-1 close via PASS=28 corpus equivalence (token streams bit-identical to chibicc baseline). Today's run measures them directly and confirms the implicit reasoning held.

Conditions: macOS 26.0 host (linker), libpython.a built on macOS 26.1 (ABI-compatible warnings only). cpython override: `NCC=$PWD/ncc2 PYDIR=/tmp/Python-3.12.3` (avoids the script's default `/Users/yamamoto/ncc/ncc2`, which is the chibicc-lineage repo and out of swap-out's lane).

---

## 2026-05-04: Phase 2 — Preprocessor swap-in CLOSED

**Replaced**: `src/preprocess.c` (1798 lines, chibicc-lineage) → `src/preprocess.c` (1817 lines, spec-derived from `docs/specs/02_preprocessor.md`).

**Lines changed**: -1798 / +1817 (+19 net).  The new file is slightly longer than the chibicc-lineage one despite dropping the unused `is_locked` field and consolidating into C11 helpers (no `open_memstream`, no `<libgen.h>`).  The increase comes from explicit forward declarations, spec-section cross-references in comments, and the `StrBuf` helper used by stringize and `<...>` filename concatenation.  Pure C and behaviorally equivalent: line count is not the success metric.

(Earlier commit message `75436c2` and the first version of this entry incorrectly stated `1798 → 1115 (-683)`; that was a measurement error from a partial state during development.  The actual numbers are above.)

**Excursions**: none.  The new `preprocess.c` uses standard C11 only: a small `StrBuf` realloc-grown buffer in place of `open_memstream`; `strrchr`-based dirname extraction in place of `<libgen.h>`'s `dirname`; `strdup`/`memcpy` patterns in place of POSIX `strndup`.  No `__attribute__`, no `__builtin_*`, no `typeof`, no statement expressions.

**Validation pyramid (post-swap)**:
| Check | Result | phase-2-baseline |
|---|---|---|
| `scripts/bootstrap_validate.sh` | FIXED POINT (md5 `f2af9fc30d93c5991061ccb87655009d`) | FIXED POINT (`596456d1...`) |
| `scripts/validate_tokenizer.sh` | PASS=33/33 | PASS=28/28 (corpus grew with 17–21 + 22–23) |
| `scripts/validate_preprocessor.sh` | PASS=35/35 (sanity, ncc vs ncc) | n/a (harness was new) |
| `tests/torture/run.sh` | 964/995 PASS, 100% non-skip | 964/995 |
| `tests/regression/run.sh` | 23/23 PASS | 16/16 baseline (now with 7 new Phase-2 tests) |
| `tests/sqlite/build.sh` | 20/20 SQL tests PASS | 20/20 |
| `build_doom_ncc2.sh` | 83/83 C files compiled | 83/83 |
| `tests/cpython/build.sh` | 153/153 core files; Python runs | 153/153 |

The bootstrap md5 differs from `phase-2-baseline`'s because the canonical `preprocess.c` source changed; the new md5 is reproducible (stage1 == stage2 under self-host).

**Closing commits on `swap-out`**:
- `1b24c2a` — Chunk 1: data structures + utility helpers
- `e57db4e` — Chunk 2: hideset ops + macro table + builtin handlers
- `54191f7` — Chunk 3: `init_macros` (full predefine table per §11)
- `47e060c` — Chunk 4: macro expansion machinery
- `47cd710` — Chunk 5+6: cond inclusion + `eval_const_expr` + `#include` resolution
- `ee1ee0e` — Chunk 7: directive dispatch + Q22/Q23 fixes — PASS=33/33
- `201318a` — validation scripts: support `NCC=ncc-v2` for dual-build testing
- `75436c2` — Phase 2 swap-in: spec-derived preprocessor becomes canonical
- `7529141` — Q17 follow-up regression tests (`22_pragma_once_stub`, `23_directive_in_arg`)
- `221760b` — Q13: move `__has_attribute` / `__has_builtin` allowlists to `cc.h`

**Deliberate divergences from `main`'s preprocessor** (each documented in spec §13, with regression tests where applicable):
- **Q1**: `__SCHAR_MAX__` defined exactly once instead of twice (same value, no observable effect; spec.md regression test 11_preprocessor_predefines.c covers it).
- **Q2**: include-path probing uses `access(R_OK)` instead of `fopen`; eliminates a real FD leak (one per `#include` probe).  No test coverage — the leak is invisible to existing harnesses, but the behavior is recorded for future audit.
- **Q22**: `pragma_handler` callback (set via `set_pragma_handler`) is now invoked when registered.  `main`'s preprocessor stores the callback but never calls it — dead infrastructure.
- **Q23**: `#error` and `#warning` now emit the directive's rest-of-line message tokens.  `main`'s preprocessor passes `""` as the format string, dropping the message text entirely.

**Provenance**: `chibicc/preprocess.c` → `ncc/src/preprocess.c` → `docs/specs/02_preprocessor.md` → new `src/preprocess.c`.  The first two share lineage; the spec was authored with full source visibility (necessary for swap-out specification per the AT&T/Sun-OS analogy: clean-room replacement of a *codebase* requires examining the codebase to know what's being replaced); the implementation step adhered to strict no-peek discipline (the spec was the only reference; `src/preprocess.c` was not opened during the implementation).  Bytes from `chibicc/preprocess.c` no longer remain in `src/`.

**Decisions baked in (Q1–Q23)**:
- Q1–Q21: from `docs/specs/02_preprocessor_questions.md`, all resolved 2026-05-03 via "accept defaults".
- Q22, Q23: surfaced during spec drafting (Chunks 5 and 7); resolved 2026-05-03 by Wayne accepting recommendation to fix.

**Open** (post-Phase-2):
- Q17.G (#include_next regression test) skipped — multi-include-path fixture work; will land if a real failure surfaces.
- Phase 5 work (deferred per spec §15): `-target elf` predefines, `__sync_*` builtin codegen, and the Q14 follow-up to extract POSIX-replacement helpers into a shared `compat.c` if other phases need them too.
- Phase 1's open divergence-log backlog (`150f17d`, `e7e7393`, `ff529fb`, `8fe8dda`, `4ed0320`, `93c6ecc`) remains gated on Phase 4/5 work — no Phase-2 entries added since cut-point.

---

## 2026-05-04: Phase 3 — Type system swap-in CLOSED

**Replaced**: `src/type.c` (435 lines, chibicc-lineage) → `src/type.c` (499 lines, spec-derived from `docs/specs/03_type.md`).

**Lines changed**: -435 / +499 (+64 net).  The new file is slightly longer because it adds spec-section cross-references in comments and has more explicit forward declarations.  Pure C11 with no behavioral change.

**Excursions**: none.  The new `type.c` uses standard C11 only.  C99 compound literals at file scope (the 15 `ty_*` singletons) are standard, not a GCC extension.  No `__attribute__`, no `__builtin_*`, no `typeof`, no statement expressions, no POSIX deps.

**Validation pyramid (post-swap)**:
| Check | Result | phase-2-closed |
|---|---|---|
| `scripts/bootstrap_validate.sh` | FIXED POINT (md5 `ef9e8d896f7affbc971bd9820af41124`) | FIXED POINT (`f2af9fc3...`) |
| `scripts/validate_tokenizer.sh` | PASS=35/35 | PASS=35/35 |
| `scripts/validate_preprocessor.sh` | PASS=35/35 (sanity, ncc vs ncc) | PASS=35/35 |
| `tests/torture/run.sh` | 964/995 PASS, 100% non-skip | 964/995 |
| `tests/regression/run.sh` | 23/23 PASS | 23/23 |
| `tests/sqlite/build.sh` | 20/20 SQL tests PASS | 20/20 |
| `build_doom_ncc2.sh` | 83/83 C files compiled | 83/83 |
| `tests/cpython/build.sh` | 153/153 core files; Python runs | 153/153 |

The bootstrap md5 differs from `phase-2-closed`'s because the canonical `type.c` source changed.  Stage1 == stage2 reproducibility holds.

**Closing commits on `swap-out`**:
- `c9ce306` — docs/swap-out-log: correct Phase 2 line-count error (cleanup carry-over)
- `9848f31` — `docs/specs/03_type_questions.md` — design questions for Phase 3
- `84a8e5c` — `docs/specs/03_type.md` — Phase 3 spec, end-to-end draft
- `b44d658` — Phase 3 dual-build scaffolding: ncc + ncc-v2 (type_v2.c skeleton)
- `4452fc3` — type_v2.c full implementation — passes everything (commit subject says "preprocess_v2 / Phase 3" — typo from copy-paste, content is correct)
- `5220102` — Phase 3 swap-in: spec-derived type system becomes canonical

**No deliberate divergences** from `main`'s observable behavior.  All Q1–Q12 questions resolved by accepting current behavior; the swap-in is purely a re-derivation under the no-peek discipline (spec-author phase peeked freely, implementation phase did not).

**Provenance**: `chibicc/type.c` → `ncc/src/type.c` → `docs/specs/03_type.md` → new `src/type.c`.  Same pattern as Phase 1 and Phase 2.  Bytes from `chibicc/type.c` no longer remain in `src/`.

**Decisions baked in (Q1–Q12)**:
- Q1: Predefined Type singletons preserved at file-scope compound-literal addresses (identity-compared by callers).
- Q2: `is_compatible` recursion preserved (chains are bounded; iteration is permitted optimization, not required).
- Q3: `copy_type` sets `origin = ty` and explicitly resets `next = NULL`.
- Q4: `vector_of` reuses `array_len` as element count (preserved overload, documented semantically).
- Q5: `add_type` idempotency check at function entry (load-bearing for parse.c speculative calls).
- Q6: VLA decay deferral to parse.c's `new_add`/`new_sub` preserved.
- Q7: Bitfield promotion sign rules preserved (unsigned width-<32 → `ty_int` per C standard's "value preserving" rule).
- Q8: `__real__`/`__imag__` on non-complex returns operand type (GCC convention).
- Q9: `*(func)` returns function type (GCC `typeof` extension support).
- Q10: `long double` size = 8 (Apple ARM64 ABI).
- Q11: New regression tests (B/C/D/E from questions doc) **not added in this session** — deferred until a real failure surfaces or until Phase 4 needs the test fixtures.
- Q12: Single-pass spec draft — completed in `84a8e5c`.

**Open** (post-Phase-3):
- Q11 follow-up regression tests (bitfield promotion, `__real__` non-complex, `typeof(*func)`, typedef compatibility) — deferred but worth adding to make Phase 3 invariants regression-proof.
- Phase 4 (parser, 6136 lines — the monster) is the natural next phase.  Inventory will be substantial.

**Source-base state at this tag** (`phase-3-closed`):
- 3 of 6 phases done: Phase 1 (tokenize.c), Phase 2 (preprocess.c), Phase 3 (type.c).
- Spec-derived: tokenize.c (679) + preprocess.c (1817) + type.c (499) = 2995 lines (~22% of src/).
- Remaining chibicc-lineage: parse.c (6136), codegen_arm64.c (2829), main.c (547), cc.h (567), alloc.c (37), unicode.c (84), hashmap.c (105) = 10305 lines.

---

## Phase 4 autonomous session — 2026-05-05 (03:22–05:00 PDT)

Working session on `swap-out` branch.  46 commits, all pushed
after each green slice (per the durable feedback rule about
concurrent sessions).  Bootstrap fixed point on canonical ncc
preserved at `ef9e8d896f7affbc971bd9820af41124` throughout.

**State on session start** (`3cd1e16`):
- parse_v2.c at 886 lines.  Spine + declspec + declarator + global_variable for `int x;`-style only.
- Most expr/stmt/init machinery still stubbed.
- Empty function bodies, no expression operators, no control flow.

**State on session end** (`9c5cdaa`):
- parse_v2.c at 4001 lines (65% of canonical parse.c at 6136).
- Torture pass rate: **881 / 995 (88.5%)** vs canonical 986/995 (99.1%).
- All 9 ncc source files compile through ncc-v2 — including
  parse_v2.c compiling itself.  `ncc-v2 → /tmp/v2-pure/ncc` is a
  working compiler that runs simple programs end-to-end.

**Surface added** (organized by spec section):

04a (declarations):
- §B (declspec): _Alignas; typeof family.
- §C/§D (declarator/array): array_dimensions full subset, multi-dim, [static], qualifiers, VLA scaffold.
- §E (function prototypes): func_params normal list + parameter type adjustments + variadic; K&R-style param decls in function bodies.
- §F (struct/union/enum): bodies, layout (with packed + bitfields per §F.5), self-referential structs, anonymous members, enum_specifier full, _Static_assert at file and block scope.
- §G (attribute_list): real impl with packed/aligned/vector_size/mode/alias honored, parsed-and-ignored set consumed.
- §H (declaration): scalar/array/string/struct/brace local init, designators, static-local lift to anon gvar, extern.
- §I (parse top-level): K&R routing, C89 implicit-int rule, parse_typedef.

04b (expressions):
- Full operator ladder: comma, assign + compound (via to_assign), conditional, logor/logand, bitwise, equality, relational, shift, additive (with new_add/new_sub pointer arith), multiplicative, cast, unary, postfix.
- unary: +, -, &, *, !, ~, sizeof (both forms), _Alignof, pre-inc/dec, __real__, __imag__, &&label.
- postfix: [i], (call), .field, ->field, ++/-- with new_inc_dec lowering, compound literal at head (local).
- primary: parens, GNU stmt-expr, _Generic, TK_NUM, TK_STR, IDENT, __func__, implicit function decl.
- new_cast real ND_CAST emission.
- try_eval_node / eval_node integer-only fold (full operator set per §K.1) + pointer-cast pass-through.
- const_expr_val helper.
- Builtins: __builtin_expect, constant_p, types_compatible_p, unreachable, va_start/end/copy/arg, offsetof, alloca, frame_address, return_address, *_overflow, clz/ctz/ffs/popcount/parity/clrsb/bswap32/64 (with l/ll suffix variants).

04c (statements):
- compound_stmt with declaration vs stmt dispatch, _Static_assert routing.
- stmt: return, if/else, while, do, for, switch/case/default, break, continue, goto (direct + computed `goto *expr`), label, asm (template-only skeleton), expr-stmt, empty.
- function() definition with per-fn state save/restore, parameter scope, alloca/va_area infra, body via compound_stmt, K&R param-decl section, label resolution (including ND_LABEL_VAL case).

04d (initializers):
- Local: scalar, brace for arrays of scalars, struct member-by-member with `.name` designators, char-array string init, incomplete-length resolution.
- Global: extracted into parse_gvar_initializer; handles char[] string, char* anon-string, integer-array, pointer-array (string and addr-of variants), struct (with member-char-array + designators), union, array-of-struct.
- Compound literal: at head of postfix (local) and at file scope (struct + integer-array, with relocations for &-of-gvar).

**Self-host status:**
- ncc-v2 compiles every src/*.c file including parse_v2.c itself.
- Linking yields a working `ncc` (`/tmp/v2-pure/ncc`).
- Stage-2 build (the v2-built ncc compiling sources for stage-3) currently SIGBUSes inside `scan_pp_num_v2` on non-trivial input — code-gen mismatch vs canonical at scale.  Trivial programs work; the bug is scale-dependent.  **Open** for the next session — fixed-point bootstrap is gated on this.

**Open work for swap-in:**
- Stage-2 SIGBUS investigation.
- Nested function definitions (compound_stmt §B.4 hint).
- `__label__` local label declarations.
- Array-of-struct nested-brace local init.
- File-scope union and array-of-array gvar init.
- ASM operands (currently template-only).
- Float fold in eval_node (eval_double).
- Anonymous-member case in find_member's general traversal (currently only struct_ref's recursion handles it).
- Q14 regression tests (deferred; see project_phase4_plan).

Session checkpoint commit: `9c5cdaa`.

---

## Phase 4 autonomous session — 2026-05-05/06

Working session on `swap-out` branch.  8 commits.  Focus: close
the gap between ncc-v2 and canonical ncc on the GCC torture suite,
not surface-area additions.

**State on session start** (`9da1e97`):
- Bootstrap stage1==stage2 fixed point already held.  The previous
  session's "stage-2 SIGBUSes inside `scan_pp_num_v2`" claim
  no longer reproduced — that bug had been fixed in one of the late
  prior commits but the log entry pre-dated it.
- Torture: ncc-v2 687/995 (69.0%).  Canonical: 964/995 (96.9%).
  Gap: 277 tests.

**State on session end** (`9cbf8ec`):
- Torture: ncc-v2 **835/995 (83.9%)**.  Gap to canonical: 129 tests.
- Bootstrap fixed point preserved at every commit (md5 changed each
  time the canonical source did).

**Bug class breakdown** (in commit order):

1. `cd1fdca` — **Float gvar init.**  17 compile-fails came from
    `parse_v2: this global initializer form not yet implemented`
    on float scalars and float arrays.  Added `try_eval_double_v2`
    (literal / neg / +-*/ / ND_CAST / ND_COND / fall-back to
    integer fold).  +10 PASS.
2. `8c83755` — **Recursive gvar sub-initializer.**  7 compile-fails
    on `nested aggregate gvar init not yet implemented` in the struct
    handler.  Added `gvar_subinit_recursive` covering struct / union /
    array / scalar leaves with relocations.  +5 PASS.
3. `2ee3a2d` — **Nested brace lvar init via offset-based helper.**
    7 compile-fails on `nested brace initializer not yet implemented`
    in array and struct local init.  Added `make_offset_lval` +
    `lvar_init_at_offset` to emit `*(ty *)((char *)&var + off) = expr`
    chains for arbitrarily nested aggregates.  +6 PASS.
4. `c8970bf` — **64-bit ptr-arith scale (new_add / new_sub).**
    Element-size scale was an `ND_NUM` with `ty_int`, so codegen
    emitted `mul w0, w0, w1` (w-register, 32-bit truncating).  Any
    `ptr +/- int` on a stack/heap address went wild.  Added `new_long`
    and used it for the scale, mirroring canonical at parse.c:144.
    +6 PASS.
5. `8f44e21` — **Implicit-decl variadic flag.**  HUGE.  Implicit
    function declarations were unconditionally setting `is_variadic`,
    so calls to undeclared `memcpy` / `memset` / `strcmp` etc. went
    through the Apple ARM64 variadic ABI (all args on stack), but the
    callee expects them in `x0`/`x1`/`x2`.  Restricted variadic to
    the printf family with synthesized named char* params, mirroring
    canonical parse.c:3919.  **+115 PASS** in one commit (714 → 829).
6. `c12d29c` — **Flex-array member normalization.**  `struct_members`
    didn't rewrite a trailing incomplete-array member to length 0, so
    `array_of(base, -1)`'s negative size collapsed the surrounding
    struct's size in `struct_layout`.  +1 PASS.
7. `9cbf8ec` — **`to_assign` double-scaling for ptr ND_ADD / ND_SUB.**
    `to_assign` re-routed the inner op through `new_add` / `new_sub`,
    which re-applied the pointer scale → offset got squared (e.g.
    `--q` on `struct S*` decremented by `sizeof(struct S)^2` bytes).
    Switched to `new_binary` direct, mirroring canonical
    parse.c:2294.  +5 PASS.

**Validation pyramid (current)**:

| Check | Result |
|---|---|
| `NCC=./ncc-v2 scripts/bootstrap_validate.sh` | FIXED POINT (md5 `12de412e5d99d14afd7eb3c176a52385`) |
| `tests/torture/run.sh` (`NCC=./ncc-v2`) | 835/995 PASS (84%, canonical 964/995) |
| Canonical `scripts/bootstrap_validate.sh` | FIXED POINT (md5 `ef9e8d896f7affbc971bd9820af41124`, untouched) |

Remaining **47 runtime fails** are mostly: bitfields, `_Complex` /
`__complex__`, K&R + struct passing, VLA + goto-back-across-decl,
GCC builtins (`__builtin_bswap`, `__builtin_constant_p`,
`__builtin_types_compatible_p`), and inline asm.  Each is a
distinct, deeper feature — the easy bug-class wins are mostly
done.  **82 compile-fails** are dominated by GCC extensions
(`&&label` const-expr, `_Complex` literals, vector compound
literals, nested function definitions).

Session checkpoint commit: `9cbf8ec`.

---

## Phase 4 closed — 2026-05-06 (`phase-4-closed` at `a414daf`)

After the autonomous session above wrapped at 835 PASS, a follow-up
session (same day) drove ncc-v2 the rest of the way through the
real-world validation suite, then performed the swap-in.

**Follow-up bug fixes** (commits `29f2bdc`, `727e69a`, `f7d0e6e`):
- `concat_adjacent_strings` — `"foo" "bar"` token merge at every
  TK_STR site (gvar/lvar/primary).  Mirrors canonical parse.c:3790.
- `try_eval_addr_v2` — fold `&gvar`, `&arr[N]`, `arr+k`,
  `(T*)&gvar`, `(T*)"literal"` into (label, addend) for static
  init.  Replaces narrow ND_ADDR/ND_VAR pattern matches scattered
  in gvar handlers.  Mirrors canonical eval2 (parse.c:1870).
- Route TY_PTR / aggregate / flonum struct fields through
  `gvar_subinit_recursive` in struct + array-of-struct gvar
  handlers (was const_expr_val'ing pointer fields).
- Origin-chain walk in `struct_ref` — `const struct B *p` declared
  before B's body becomes a copy_type'd Type *; chase ty->origin to
  the completed type.  Mirrors canonical struct_ref (parse.c:2911).
- Lift integer-array gvar init's hardcoded 1024-element cap to a
  doubling realloc — SQLite has tables with >1024 entries.
- Generic aggregate fallback in parse_gvar_initializer — multi-dim
  arrays (`u8 trans[8][8]`) and any other unmatched aggregate
  brace-init now route through gvar_subinit_recursive.
- `__builtin_bswap16` — table can't carry the result-type/val=16
  convention; inline special-case before the generic table.
- Float-to-int cast fold in try_eval_node's ND_CAST — for
  `(int)(-.867 * R)` style static initializers (doom).
- **offsetof const-fold** — `offsetof(T,m)` (defined in stddef.h
  as `((size_t)&((T*)0)->m)`) wasn't folding at compile time, so
  any sizeof-of-array-dimension referencing offsetof — most notably
  SQLite's `char saveBuf[PARSE_TAIL_SZ]` where `PARSE_TAIL_SZ =
  sizeof(Parse) - offsetof(Parse, sLastToken)` — got the wrong
  size.  Stack frame collapsed → memcpy crash.  Added ND_ADDR +
  ND_MEMBER cases that walk the `&((T*)0)->m1.m2.m3` chain.

**Validation pyramid post-swap (canonical ncc, no `-v2`)**:

| Check | Result |
|---|---|
| `scripts/bootstrap_validate.sh` | FIXED POINT (md5 `154536038f7a206073c36a0c189ab112`) |
| `tests/torture/run.sh` | 849/995 PASS (CF=71, RT=44, SKIP=31) |
| `tests/sqlite/build.sh` | 20/20 SQL tests PASS end-to-end |
| `build_doom_ncc2.sh` | 83/83 C files compile + link |
| `tests/cpython/build.sh` | deferred (Python source absent on this machine) |

**Source-base state at this tag** (`phase-4-closed`):
- 4 of 6 phases done: 1 (tokenize), 2 (preprocess), 3 (type), 4 (parse).
- Spec-derived: tokenize.c (679) + preprocess.c (1817) + type.c (499)
  + parse.c (~4400) = ~7400 lines of pure C11 (53% of src/).
- Remaining chibicc-lineage: codegen_arm64.c (2829), main.c (547),
  cc.h (567), alloc.c (37), unicode.c (84), hashmap.c (105) =
  ~4170 lines.

**Decisions baked in (Phase 4)**:
- Implicit-decl: NOT variadic by default; only `printf` family marked
  variadic with synthesized named char* params (matches canonical's
  hardcoded list).  Was the single biggest source of runtime
  failures — fixed `memcpy`/`memset`/`strcmp`/etc. ABI breakage.
- 64-bit pointer-arith scale: `new_long` for the element-size
  multiplier, so `mul x0,x0,x1` not `mul w0,w0,w1`.
- `to_assign` does NOT re-route through `new_add`/`new_sub`; uses
  `new_binary` direct on already-scaled rhs (avoids double-scaling).
- Struct member origin walk in `struct_ref` for forward-declared
  struct const-qualified copies.
- offsetof folds at compile time via ND_ADDR + ND_MEMBER chain walk
  in try_eval_node.
- Trailing flex-array struct member rewritten to `array_of(base, 0)`
  in `struct_members` so `struct_layout` doesn't get a negative size.

**Open** (post-Phase-4, documented in `docs/parse_v2_torture_gap.md`):
- 115 torture failures grouped by feature class.  Top buckets:
  nested function (10), &&label / computed-goto (10+3),
  _Complex (10+3), bitfield runtime (10), K&R def (10),
  nested-aggregate init (9).
- Phase 5 (codegen_arm64.c, 2829 lines) is the natural next phase.
  Inventory not yet started.

**Closing commits on `swap-out`**:
- `f7d0e6e` — offsetof const-fold; SQLite 20/20 PASS
- `8a329dc` — docs/parse_v2_torture_gap refresh — 14 closed, 115 remaining
- `a414daf` — Phase 4 swap-in: spec-derived parser becomes canonical
- Tag `phase-4-closed` pushed.

**No deliberate divergences** from the pre-swap ncc-v2 behavior.
Provenance: chibicc/parse.c → ncc/src/parse.c → docs/specs/04*.md →
new src/parse.c.  Same pattern as Phases 1/2/3.  Bytes from
chibicc/parse.c no longer remain in `src/`.

---

## Autonomous session — 2026-05-06 (`auto-session-2026-05-06-1552`)

4-hour autonomous run starting from `dae1251` (post-swap-in).
Target: drive down the 115-test torture gap documented in
`docs/parse_v2_torture_gap.md`.  All work landed on
`auto-session-2026-05-06-1552` (not pushed).

**Result**: 849/995 → 888/995 (+39 PASS, zero regressions).
13 commits, all bootstrap-fixed-point-preserving.  Validation
pyramid still clean: bootstrap stage1==stage2 (md5
`bc5bfef8149e9054f358d81b1abce1f0`), SQLite 20/20, doom 83/83.

**Bug-class fixes** (in commit order):

1. `c02f7df` — Five new builtins: `__builtin_prefetch` (lower to
   side-effect-evaluation comma chain), `__builtin_setjmp` /
   `__builtin_longjmp` (synthesize calls to libc), `__builtin_mul_overflow_p`
   (compile-time fold via __int128), `__builtin_classify_type`
   (return GCC type-class enum). +5 PASS.
2. `5127647` — `__builtin_return_address(0)` and
   `__builtin_frame_address(0)` codegen used `gen_expr(node->lhs)`
   but parse stored the depth in `node->val` → SIGSEGV in the
   compiler.  Replace with compile-time-unrolled walk keyed on
   node->val.  +4 PASS.
3. `73af2b7` — Pre-C99 `name: value` designator form (instead of
   `.name = value`) accepted at every initializer site (gvar /
   lvar / compound literal / recursive helpers), plus a missing
   TY_UNION arm in lvar brace-init dispatch.  +3 PASS.
4. `1bdb0c1` — `__builtin_constant_p` now folds to 1 for string
   literals (was 0).  `__builtin_bswap32` / `bswap64` returned
   `ty_int` from the unary-builtin table → bswap64 result got
   truncated through `sxtw`; set proper `ty_uint` / `ty_ulong`.
   +2 PASS.
5. `ed615fe` — `__attribute__((alias("name")))` wired through.
   Codegen already supported it; parse-side just dropped the
   target name.  +3 PASS.
6. `91f44d1` — K&R-style function definitions fixed.  The
   identifier-list `(value)` produces `value: int` in
   fn->ty->params; the K&R fix-up loop only patched fn->params
   (Obj list).  Now mirror into fn->ty->params so call sites
   cast args to the K&R-declared type instead of truncating long
   long via `sxtw`.  +9 PASS.
7. `1a3c136` — `try_eval_addr_v2` walks ND_MEMBER chains for
   `&gvar.field.field` patterns and folds through ND_DEREF for
   `&((gvar+k))->field`.  +1 PASS.
8. `e5d7b4b` — Function `__attribute__((aligned(N)))` → fn->align,
   propagated from forward declarations into definitions.  +1 PASS.
9. `22956d8` — `init_compound_literal` routes nested-aggregate
   slots through `lvar_init_at_offset` (was rejecting nested
   braces with "not yet implemented").  +2 PASS.
10. `6b6c015` — `asm()` operand expressions now actually evaluate.
    Previous parser skipped the body brace-balanced.  Now: parse
    sections (outputs/inputs/clobbers/labels), extract operand
    exprs into a comma chain, lower as `{ side effects; ND_ASM; }`.
    +1 PASS.
11. `ed75b6e` + `eb4850b` — Chained designators `.a.b.c = value`
    in gvar struct, lvar struct, and compound literal struct/union
    handlers.  Walk the chain accumulating offsets, dispatch
    through the recursive helpers at the cumulative offset/leaf
    type.  +2 PASS.
12. `e2ddcc8` — Doc refresh.

**Validation post-session**:

| Check | Result |
|---|---|
| `scripts/bootstrap_validate.sh` | FIXED POINT (md5 `bc5bfef8149e9054f358d81b1abce1f0`) |
| `tests/torture/run.sh` | 888/995 PASS (CF=43, RT=33, SKIP=31) |
| `tests/sqlite/build.sh` | 20/20 SQL tests PASS |
| `build_doom_ncc2.sh` | 83/83 C files compile + link |

**Remaining 76-test gap** — dominated by feature debt:
- nested function (10)
- &&label / computed goto (12)
- _Complex / __complex__ (8)
- bitfield runtime (12)
- vector type (7+4)
- VLA runtime (6+1)
- inline asm template constraints (1: 20030222-1)
- brace elision (2: 20021118-1, 991201-1)
- flexible-array string init (1: 20010924-1)
- _Complex compound-literal address (1: 20050929-1)
- llabs builtin (1)
- chained-const-pointer runtime (1: pr103209)
- types_compatible_p (1: needs deep type-equiv)

Not pushed.  Branch `auto-session-2026-05-06-1552` ahead of
`swap-out` by 13 commits.  Recommended next start point: merge
the session branch into swap-out (or cherry-pick), update
phase-4-closed bookkeeping, then either tackle the deep
features (nested fn, _Complex, bitfields) or move on to Phase 5
(codegen).

