/* SPEC §3.2 — constant folding must respect FP operand types.
   Every condition below is a compile-time-constant FP comparison
   whose true value is false. */

int main(void) {
    if ((double)1.0 > 2.0) return 1;
    if (1.5 < 0.5) return 2;
    if (-1.0 > 0.0) return 3;
    if ((float)2.5f < (float)1.5f) return 4;
    if ((double)18446744073709551615ULL < 1.84467440737095e+19) return 5;
    return 0;
}
