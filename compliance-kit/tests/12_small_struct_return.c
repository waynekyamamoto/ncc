/* AAPCS64: small struct (<=16 bytes) return is in x0/x1. */

struct two_longs { long a; long b; };
struct three_ints { int a; int b; int c; };

static struct two_longs make_two(long x) {
    struct two_longs r = { x, x * 2 };
    return r;
}

static struct three_ints make_three(int x) {
    struct three_ints r = { x, x + 1, x + 2 };
    return r;
}

int main(void) {
    struct two_longs t = make_two(100);
    if (t.a != 100 || t.b != 200) return 1;

    struct three_ints u = make_three(10);
    if (u.a != 10 || u.b != 11 || u.c != 12) return 2;

    return 0;
}
