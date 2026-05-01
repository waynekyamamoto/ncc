// Compiler attributes that NetBSD kernel sources use but ncc treats as
// no-ops.  The compiler must accept the syntax and ignore the body
// without warning.  If a future compiler emits warnings or errors here,
// the kernel build breaks.

#include <stdio.h>

__attribute__((target("arch=armv8.4-a")))
static int target_fn(int x) { return x + 1; }

__attribute__((__pcs__("aapcs")))
static int pcs_fn(int x) { return x + 2; }

__attribute__((no_sanitize_address))
static int nsa_fn(int x) { return x + 3; }

__attribute__((no_sanitize("undefined")))
static int nsu_fn(int x) { return x + 4; }

__attribute__((no_instrument_function))
static int ninstr_fn(int x) { return x + 5; }

int main(void) {
    int s = target_fn(0) + pcs_fn(0) + nsa_fn(0) + nsu_fn(0) + ninstr_fn(0);
    printf("%d\n", s);
    return s == (1 + 2 + 3 + 4 + 5) ? 0 : 1;
}
