// Static __inline void with single __asm__ body should be inlined at the call
// site so that compile-time-constant arguments flow into "n" / "i" constraints.
// This is the pattern NetBSD uses for PSTATE writes (msr daifset, #imm).
//
// We can't issue privileged instructions from user space, so instead we use a
// pattern where the inlined asm produces a known value via a register move
// from an immediate constant. If inlining works, the literal flows in; if it
// doesn't, ncc would either generate wrong code or fail.

#include <stdio.h>

static __inline void __attribute__((__always_inline__))
mov_imm(unsigned long *dst, const unsigned long src) {
    // "i" / "n" require src to be a compile-time constant. With inlining,
    // src is the literal passed at the call site; without inlining, it's a
    // runtime parameter and the assembler will error out.
    __asm volatile ("mov %0, %1" : "=r"(*dst) : "i"(src));
}

int main(void) {
    unsigned long a = 0, b = 0;
    mov_imm(&a, 0x1234);
    mov_imm(&b, 0xabcd);
    printf("%lx %lx\n", a, b);
    return 0;
}
