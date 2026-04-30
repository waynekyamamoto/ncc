// Regression: peephole `mov rN, rN` redundant-mov drop only safe for
// X-registers, NOT W-registers.
//
// On AArch64 a 32-bit W-register write zero-extends to the 64-bit X
// register. So `mov w0, w0` is NOT a no-op — it clears the upper 32
// bits of x0. The peephole optimizer (added by 5bf1474) was matching
// the textual `mov rN, rN` regardless of which register family, and
// dropping `mov w0, w0` lost the implicit zero-extension. The
// canonical break: gcc-torture/20140622-1.c — `(long)(p + a) - (long)p`
// for unsigned p, a. The unsigned add evaluates at 32 bits, then the
// `(long)` cast widens via a `mov w0, w0` zero-extension that ncc
// emits and peephole was incorrectly dropping; the result was sign-
// extended instead of zero-extended.
//
// Fix: is_redundant_mov() now requires the register to start with
// 'x' (X-form, 64-bit, true no-op).
//
// Fixed: 2026-05-04, alongside the re-land of the peephole pass.

unsigned p;

long __attribute__((noinline)) test(unsigned a) {
  return (long)(p + a) - (long)p;
}

int main(void) {
  p = (unsigned)-2;
  if (test(0) != 0) return 1;
  if (test(1) != 1) return 2;
  if (test(2) != -(long)(unsigned)-2) return 3;
  p = (unsigned)-1;
  if (test(0) != 0) return 4;
  if (test(1) != -(long)(unsigned)-1) return 5;
  if (test(2) != -(long)(unsigned)-2) return 6;
  return 0;
}
