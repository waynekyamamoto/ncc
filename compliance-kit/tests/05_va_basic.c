/* SPEC §2 — variadic baseline: a few int args via stdarg. */

#include <stdarg.h>

static int sum(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}

int main(void) {
    if (sum(3, 10, 20, 30) != 60) return 1;
    if (sum(1, 42) != 42) return 2;
    if (sum(0) != 0) return 3;
    return 0;
}
