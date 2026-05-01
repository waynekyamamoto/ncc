// Regression: ND_EQ in eval2 strictly required both operands to evaluate
// as pure integers, so comparing a static address against a null pointer
// constant in a constant initializer failed with "not a compile-time
// constant". GCC's _OF_DECLARE family in the Linux kernel relies on
// this idiom to typecheck a function pointer at compile time:
//
//     .data = (fn == (fn_t)NULL) ? fn : fn
//
// Both arms of the ternary are the same `fn`; the comparison only exists
// to force GCC to typecheck `fn` against `fn_t`. ncc rejected this as a
// constant initializer, blocking ~22 drivers/clk/* files (and other
// _OF_DECLARE users elsewhere in the kernel).
//
// Fix: in eval2, treat (static_addr == 0) as compile-time false.
// Found via: scripts/linux_scan/scan.sh on drivers/clk.
// Fixed: 2026-04-29.

typedef void (*fn_t)(int);

static void my_init(int x) { (void)x; }

struct entry {
    const char *name;
    fn_t data;
};

static const struct entry e = {
    .name = "x",
    .data = (my_init == (fn_t)0) ? my_init : my_init,
};

int main(void) { return 0; }
