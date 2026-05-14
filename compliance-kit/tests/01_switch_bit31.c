/* SPEC §3.1 — case constants must be represented as 64-bit signed
   integers; a label with bit 31 set must match an equal unsigned
   switch expression. */

int main(void) {
    unsigned x = 0x80047410U;
    switch (x) {
    case 0x80047410U: return 0;
    default:          return 1;
    }
}
