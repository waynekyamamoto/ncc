// Take address of a file-scope global (in .data, far from .text), call
// through it, dereference fields.  Stresses ADRP+ADD:lo12 for high-VA
// addresses and that the runtime address matches the linker-resolved
// one.  An earlier (incorrect) hypothesis blamed this path for the
// kern_sysctl boot bug; that hypothesis was wrong but the test value
// stands — it locks in correct codegen for a pattern the kernel uses
// pervasively.

#include <stdio.h>

static int callback_int(void *p) {
    (void)p;
    return 0xCAFE;
}

struct timecounter_like {
    int (*get)(void *);
    int quality;
    char name[8];
};

static struct timecounter_like tc = {
    .get = callback_int,
    .quality = -100,
    .name = "tc0",
};

static struct timecounter_like *volatile tc_p = &tc;

int main(void) {
    struct timecounter_like *p = tc_p;
    int v = p->get(0);
    printf("%d %d %s\n", v, p->quality, p->name);
    return (v == 0xCAFE && p->quality == -100) ? 0 : 1;
}
