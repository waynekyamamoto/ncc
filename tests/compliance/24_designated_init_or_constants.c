// File-scope struct designated-initializer with the RHS being a bitwise
// OR of multiple integer constants.  Verified manually during the
// kern_sysctl.c diagnosis (an earlier hypothesis suspected this path);
// this test locks in the existing correct behavior so a rewrite can't
// regress it without noticing.

#include <stdio.h>

#define V_VERSION   0x01000000
#define V_FLAG_A    0x00000001
#define V_FLAG_B    0x00000200
#define V_FLAG_C    0x00002000

struct s {
    unsigned int flags;
    int n;
    char name[8];
};

static struct s g = {
    .flags = V_VERSION | V_FLAG_A | V_FLAG_B | V_FLAG_C,
    .n = -42,
    .name = "ok",
};

int main(void) {
    unsigned int expected = 0x01000000u | 0x00000001u | 0x00000200u | 0x00002000u;
    printf("%u %d %s\n", g.flags, g.n, g.name);
    return (g.flags == expected && g.n == -42) ? 0 : 1;
}
