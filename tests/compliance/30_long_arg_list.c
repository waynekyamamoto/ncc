// Function with 12 named int parameters (no variadic) — sanity for
// AAPCS64 stack-arg passing without the variadic complication.  The
// last 4 params are caller-stack-passed.  Both caller-side push and
// callee-side load must use the right offsets.

#include <stdio.h>

static int sum12(int a1, int a2, int a3, int a4,
                 int a5, int a6, int a7, int a8,
                 int a9, int a10, int a11, int a12) {
    /* Touch each parameter so the compiler can't optimize them away. */
    return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12;
}

int main(void) {
    int s = sum12(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    printf("%d\n", s);
    /* Expected: 78 (gauss formula 12*13/2) */
    return s == 78 ? 0 : 1;
}
