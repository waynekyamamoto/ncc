/* SPEC §2.4 — variadic doubles. 4 args exercise the VR save area;
   10 args force the FP overflow path. */

#include <stdarg.h>

static double sum_d(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double s = 0.0;
    for (int i = 0; i < n; i++) s += va_arg(ap, double);
    va_end(ap);
    return s;
}

int main(void) {
    double r = sum_d(4, 1.5, 2.5, 3.5, 4.5);
    if (r != 12.0) return 1;

    r = sum_d(10, 1.0, 2.0, 3.0, 4.0, 5.0,
                  6.0, 7.0, 8.0, 9.0, 10.0);
    if (r != 55.0) return 2;
    return 0;
}
