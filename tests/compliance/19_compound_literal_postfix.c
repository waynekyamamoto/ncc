// Compound literal `(type){...}` followed by `.member`, `[idx]`, or `->m`.
// Previously ncc's compound-literal path returned early before the
// postfix-suffix loop, so `.tag` after the literal wasn't parsed.
// ACPICA exercises this with `(union {...}){ x }._t`.

#include <stdio.h>

int main(void) {
    // 1. Compound literal followed by member access on anon union.
    int v1 = ((const union { int _i; char _c[4]; }){ ._i = 0xcafe })._i;

    // 2. Compound literal followed by subscript.
    int v2 = ((int[]){ 10, 20, 30, 40 })[2];

    // 3. Compound literal of struct followed by .field.
    struct point { int x, y; };
    int v3 = ((struct point){ .x = 5, .y = 7 }).y;

    // 4. Address-of compound literal followed by ->.
    int v4 = (&((struct point){ .x = 100, .y = 200 }))->x;

    printf("%x %d %d %d\n", v1, v2, v3, v4);
    return 0;
}
