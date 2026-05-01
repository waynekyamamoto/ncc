// Function returning a struct.  AAPCS64: structs ≤ 16 bytes returned
// in registers (x0/x1 for ints, d0/d1 for floats), structs > 16 bytes
// returned via caller-supplied indirect pointer in x8.  This test
// covers a small struct returned in registers — the most common case
// and one the kernel uses extensively.

#include <stdio.h>

struct point {
    int x;
    int y;
};

static struct point make_point(int a, int b) {
    struct point p;
    p.x = a;
    p.y = b;
    return p;
}

int main(void) {
    struct point p = make_point(7, 11);
    printf("%d %d\n", p.x, p.y);
    return (p.x == 7 && p.y == 11) ? 0 : 1;
}
