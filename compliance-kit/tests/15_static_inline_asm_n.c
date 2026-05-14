/* SPEC §4.4 — `static inline void` with a single `__asm__` body
   that uses the "n" (immediate constant) constraint. A
   compile-time-constant call argument must reach the asm template
   as an immediate. Side-effect channel: writes the immediate to a
   volatile global via a scratch register. */

volatile unsigned sink;

static inline void emit_const(unsigned v) {
    __asm__ volatile(
        "mov w9, #%c0\n\t"
        "str w9, %1"
        :: "n"(v), "m"(sink)
        : "w9", "memory");
}

int main(void) {
    emit_const(42);
    if (sink != 42) return 1;
    emit_const(0);
    if (sink != 0) return 2;
    emit_const(255);
    if (sink != 255) return 3;
    return 0;
}
