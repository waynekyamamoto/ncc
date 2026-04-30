// Forward-reference to a function used in a file-scope struct initializer.
// NetBSD's kern_cctr.c uses this pattern: a function prototype is provided
// (typically via an included header), the struct of function pointers is
// initialized at file scope, and the function body is defined later in the
// same TU.
//
// Both clang and gcc accept this when a prototype is in scope. ncc must too —
// the in_gvar_initializer path must NOT regress this case (it's the common
// scenario; the no-prototype case is the rarer extension).

#include <stdio.h>

typedef unsigned int (*getcc_t)(void *);

struct timecounter {
    getcc_t tc_get_timecount;
    int tc_quality;
};

// Prototype declared BEFORE the struct initializer, definition AFTER.
unsigned int cc_get_timecount(void *);

static struct timecounter cc_timecounter = {
    .tc_get_timecount = cc_get_timecount,
    .tc_quality = -100000,
};

unsigned int
cc_get_timecount(void *tc)
{
    (void)tc;
    return 0xDEADBEEF;
}

int main(void) {
    printf("%u %d\n",
           cc_timecounter.tc_get_timecount((void *)0),
           cc_timecounter.tc_quality);
    return 0;
}
