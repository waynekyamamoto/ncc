/* SPEC §4.3 — parse-and-ignore `target`, `pcs`, `no_sanitize`
   attributes. */

__attribute__((target("default")))
static int f1(void) { return 1; }

__attribute__((pcs("aapcs")))
static int f2(void) { return 2; }

__attribute__((no_sanitize("address")))
static int f3(void) { return 3; }

__attribute__((no_sanitize("address", "undefined")))
static int f4(void) { return 4; }

int main(void) {
    if (f1() != 1) return 1;
    if (f2() != 2) return 2;
    if (f3() != 3) return 3;
    if (f4() != 4) return 4;
    return 0;
}
