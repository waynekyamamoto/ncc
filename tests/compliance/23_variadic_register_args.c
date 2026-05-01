// Variadic where ALL variadic args fit in registers (function has only
// 1 named arg).  AArch64 calling convention: x0 holds named arg, x1..x7
// hold the first 7 variadic ints, x29+16 onward holds variadic args 8+.
//
// For va_arg to work in this case, the prologue must save x1..x7 to a
// register save area on the stack and `va_start` must aim `ap` there.
// The current chibicc-derived ncc does NOT build a register save area
// (it only handles the stack-spilled-named-overflow case fixed in
// commit 579f490).  This test is therefore expected to fail under the
// current compiler — it's the regression bar for the rewrite.

#include <stdarg.h>
#include <stdio.h>

static int sum_first_three(int dummy, ...) {
    va_list ap;
    va_start(ap, dummy);
    int a = va_arg(ap, int);
    int b = va_arg(ap, int);
    int c = va_arg(ap, int);
    va_end(ap);
    (void)dummy;
    return a + b + c;
}

int main(void) {
    int s = sum_first_three(0, 100, 200, 300);
    printf("%d\n", s);
    return s == 600 ? 0 : 1;
}
