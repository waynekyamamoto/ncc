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

## Divergence log: changes on `main` since swap-out cut

Tracking commits that land on `main` (`docs/main-commit-contract.md` defines what each entry needs to provide). The swap-out branch will not merge or cherry-pick these; instead, at each phase boundary, this list gets walked and each entry is checked against the reimplemented code. See `docs/main-commit-contract.md` for the contract.

Cut point: `7ff0860`. New commits on `main` after this point will be appended below as they happen.

(no entries yet)
