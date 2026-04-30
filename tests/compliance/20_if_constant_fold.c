// `if (constant)` should fold at codegen, emitting only the taken branch.
// ncc uses try_eval_node to handle `!__builtin_constant_p(x)` patterns.
// NetBSD's daif_disable() relies on this — the `else` branch contains a
// `msr daifset, %0` with a register operand that the assembler rejects;
// folding away that branch is what makes the kernel build.

#include <stdio.h>

static inline int test(int x) {
    // For non-constant x, __builtin_constant_p(x) returns 0, so the
    // else branch is dead. ncc must NOT emit it (or the compiler-aware
    // assembler/linker would object — here we just check both paths
    // produce the right value).
    if (!__builtin_constant_p(x)) {
        return x * 2;
    } else {
        return x * 3;
    }
}

int main(void) {
    int n = 7;
    printf("%d %d\n", test(n), test(11));
    return 0;
}
