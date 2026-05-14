/* AAPCS64 stack-passing baseline: 12 named int params (no variadic).
   a1..a8 in x0..x7, a9..a12 on stack. Distinct values catch any
   swapped slot. */

static int test(int a1, int a2, int a3, int a4,
                int a5, int a6, int a7, int a8,
                int a9, int a10, int a11, int a12) {
    if (a1 != 1) return 1;
    if (a2 != 2) return 2;
    if (a3 != 3) return 3;
    if (a4 != 4) return 4;
    if (a5 != 5) return 5;
    if (a6 != 6) return 6;
    if (a7 != 7) return 7;
    if (a8 != 8) return 8;
    if (a9 != 9) return 9;
    if (a10 != 10) return 10;
    if (a11 != 11) return 11;
    if (a12 != 12) return 12;
    return 0;
}

int main(void) {
    return test(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
}
