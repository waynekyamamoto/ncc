/* SPEC §2 — va_copy: two walkers visiting the same arg list must
   produce identical results. */

#include <stdarg.h>

static int walk(int n, va_list ap) {
    int s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    return s;
}

static int twice(int n, ...) {
    va_list ap, ap2;
    va_start(ap, n);
    va_copy(ap2, ap);
    int s1 = walk(n, ap);
    int s2 = walk(n, ap2);
    va_end(ap);
    va_end(ap2);
    if (s1 != s2) return -1;
    return s1;
}

int main(void) {
    int r = twice(5, 1, 2, 3, 4, 5);
    if (r < 0) return 1;
    if (r != 15) return 2;
    return 0;
}
