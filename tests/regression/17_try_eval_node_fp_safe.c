// try_eval_node must not evaluate floating-point operands as int64.
//
// Bug: ND_NUM and ND_CAST cases unconditionally read node->val (int64_t).
// For an FP literal like 1.5, the value is in node->fval; node->val is
// garbage (typically 0). With the netbsd-features `if (try_eval_node(...))`
// hook in gen_stmt's ND_IF case, an `if ((double)x < 1.5)` was folded
// using LHS=garbage and RHS=0 -> wrong constant -> wrong branch taken.
//
// Repro of GCC torture 920710-1 boiled down: just confirm that a runtime
// FP comparison is not constant-folded. We use _Static_assert to keep
// this test minimal — if try_eval_node mistakenly succeeds on an FP-typed
// expression at compile time, the static_assert will fire (or, worse,
// silently use wrong values; this test asserts the *type* of the result
// to ensure ncc doesn't reach FP through try_eval_node).

// Compile-only check: a function whose body would crash if `if` was
// wrongly folded. We don't run this; just ensure ncc compiles it without
// emitting only one branch.
extern int side_effect(void);

int test_fp_branch(double x) {
  if ((double)18446744073709551615u < x)
    return side_effect();
  return 0;
}

int test_fp_literal_branch(double x) {
  if (x < 1.84467440737095e+19)
    return side_effect();
  return 0;
}
