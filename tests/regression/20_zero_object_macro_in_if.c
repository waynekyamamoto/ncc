// Q17.F — `#if` evaluates its argument as a constant expression after
// macro expansion.  An object-like macro that expands to `0` must cause
// the controlled block to be skipped.
//
// This test catches a subtle bug: if `#if` were to test "is the macro
// defined" rather than "evaluate the expanded value," then `#if ZERO`
// would take the THEN branch (since ZERO is defined).  The correct
// behavior is to expand ZERO to its value (0), then evaluate the result
// (0 is false).
//
// Test case from the inventory's "Notes for spec author" #11:
// `#define ZERO 0\n#if ZERO\n` should NOT include the block.
//
// Spec ref: docs/specs/02_preprocessor.md §8.1 (#if) + §9 (eval_const_expr),
//           docs/specs/02_preprocessor_questions.md inventory recap.

#define ZERO 0

#if ZERO
#error "#if ZERO took the THEN branch; ZERO=0 must evaluate as false"
#endif

#define ONE 1

#if !ONE
#error "#if !ONE took the THEN branch; ONE=1 must evaluate as true"
#endif

// Macro that expands to a more complex zero-valued expression.
#define COMPLEX_ZERO (1 - 1)

#if COMPLEX_ZERO
#error "#if COMPLEX_ZERO took the THEN branch; (1-1)=0 must evaluate as false"
#endif

int main(void) { return 0; }
