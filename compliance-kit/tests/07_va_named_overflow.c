/* SPEC §2.3 — when named GP params spill onto the stack, variadic
   args start after the named overflow slots. 12 named ints + 5
   variadic ints. */

#include <stdarg.h>

static int test(int a1, int a2, int a3, int a4, int a5, int a6,
                int a7, int a8, int a9, int a10, int a11, int a12,
                ...) {
    if (a1 != 1 || a2 != 2 || a3 != 3 || a4 != 4 ||
        a5 != 5 || a6 != 6 || a7 != 7 || a8 != 8 ||
        a9 != 9 || a10 != 10 || a11 != 11 || a12 != 12)
        return -1;

    va_list ap;
    va_start(ap, a12);
    int s = 0;
    for (int i = 0; i < 5; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}

int main(void) {
    int r = test(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                 100, 200, 300, 400, 500);
    if (r == -1) return 1;
    if (r != 1500) return 2;
    return 0;
}
