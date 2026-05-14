/* SPEC §3.3 — `mov xN, xN` is a no-op; `mov wN, wN` zero-extends
   the low 32 bits and must not be elided. Exercises unsigned->long
   widening through a non-inlinable call. */

unsigned p, a;

static unsigned __attribute__((noinline)) opaque(unsigned v) { return v; }

int main(void) {
    p = 0;
    a = opaque(0x80000000U);

    long widened = (long)(unsigned)a;
    if (widened != 0x80000000L) return 1;

    long d = (long)(p + a) - (long)p;
    if (d != (long)(unsigned)a) return 2;

    return 0;
}
