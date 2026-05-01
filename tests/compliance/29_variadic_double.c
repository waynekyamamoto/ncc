// Variadic function with double args.  Exercises the FP-side overflow
// path of the va_area calculation: when more than 8 fp-passable named
// params spill to stack, va_area must skip them.  Less common in the
// kernel than the int-side, but the codegen path exists.
//
// Also: per the C ABI, float arguments to variadic functions are
// promoted to double.

#include <stdarg.h>
#include <stdio.h>

static double sum_doubles(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double total = 0.0;
    for (int i = 0; i < n; i++)
        total += va_arg(ap, double);
    va_end(ap);
    return total;
}

int main(void) {
    double s = sum_doubles(4, 1.5, 2.25, 3.125, 4.0625);
    printf("%.4f\n", s);
    /* Expected: 10.9375 */
    return (s > 10.93 && s < 10.95) ? 0 : 1;
}
