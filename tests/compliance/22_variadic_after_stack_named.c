// va_arg after >8 named int args.  NetBSD's sysctl_createv uses this:
// 12 named ints, then variadic.  ncc's va_start used to point ap at the
// first stack-spilled NAMED arg (a9) instead of past a12, so va_arg read
// back named args 9-12 in place of variadic args, breaking sysctl_init
// and blocking kernel boot.  Fixed in codegen_arm64.c emit_text.

#include <stdarg.h>
#include <stdio.h>

static int read_first_variadic(int a1, int a2, int a3, int a4,
                               int a5, int a6, int a7, int a8,
                               int a9, int a10, int a11, int a12, ...) {
    va_list ap;
    va_start(ap, a12);
    int v = va_arg(ap, int);
    va_end(ap);
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;
    (void)a9;(void)a10;(void)a11;(void)a12;
    return v;
}

int main(void) {
    int x = read_first_variadic(1,2,3,4,5,6,7,8, 9,10,11,12, 100);
    printf("%d\n", x);
    return x == 100 ? 0 : 1;
}
