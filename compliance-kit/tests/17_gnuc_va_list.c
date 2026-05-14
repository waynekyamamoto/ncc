/* SPEC §4.1 — `__gnuc_va_list` must be available as a typedef
   equivalent to `__builtin_va_list` / `va_list`. */

#include <stdarg.h>

static int walk(int n, __gnuc_va_list ap) {
    int s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    return s;
}

static int caller(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int r = walk(n, ap);
    va_end(ap);
    return r;
}

int main(void) {
    if (caller(3, 1, 2, 3) != 6) return 1;
    return 0;
}
