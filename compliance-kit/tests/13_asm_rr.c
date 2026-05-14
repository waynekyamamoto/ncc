/* Inline asm round-trip with "=r" / "r" register constraints,
   AArch64. */

int main(void) {
    int x = 42, y = 0;
    __asm__("mov %w0, %w1" : "=r"(y) : "r"(x));
    if (y != 42) return 1;

    long lx = 0x123456789aL, ly = 0;
    __asm__("mov %0, %1" : "=r"(ly) : "r"(lx));
    if (ly != 0x123456789aL) return 2;

    return 0;
}
