// Inline asm with an immediate-constraint operand whose value is a
// compile-time constant.  NetBSD's kernel relies on this for PSTATE
// writes (`__asm("msr daifset, %0" :: "n"(val))` etc.) — the assembler
// needs to see a literal `#8`, not a register.  ncc handles this via
// the "mini-inliner": a static-inline function with a single asm stmt
// gets spliced at every call site, so call-time constants flow through
// to the asm template's "n" constraint.

#include <stdio.h>

/* Add 1 to x via inline asm using a literal-only "i" constraint.
 * We're not testing assembler integration with the host (that's clang
 * vs aarch64-elf-as), just that ncc accepts the syntax and produces
 * code that runs.  Using a #-immediate add keeps it simple. */
static inline int add_imm(int x, int n) {
    int r;
    __asm__("add %w0, %w1, #1" : "=r"(r) : "r"(x));
    (void)n;
    return r;
}

int main(void) {
    int a = add_imm(41, 1);
    printf("%d\n", a);
    return a == 42 ? 0 : 1;
}
