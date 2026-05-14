/* SPEC §2.2 — variadic GP register path: 1 named int + variadic
   longs that fill the GP register save area and then spill into the
   stack overflow area. */

#include <stdarg.h>

static long sum_l(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, long);
    va_end(ap);
    return s;
}

int main(void) {
    long r = sum_l(8, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L);
    if (r != 36L) return 1;
    r = sum_l(10, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 9L, 10L);
    if (r != 55L) return 2;
    return 0;
}
