// __attribute__((section("name"))) on a global should place it in the named
// section. Linker still binds the address normally, so reading the variable
// works exactly like a regular global. This is the pattern NetBSD uses for
// link sets (sysctl_setup, evcnt registration, kthread autoboot).

#include <stdio.h>

static int payload = 0xdead;

// Place a pointer to `payload` in a custom section. On macOS the section name
// must include both segment and section: "__DATA,custom_set". On ELF a single
// name like "custom_set" is enough. We exercise compilation; runtime layout
// only matters for the linker scripts that aggregate link sets, which is
// outside ncc's job.
#ifdef __APPLE__
__attribute__((section("__DATA,custom_set"), used))
#else
__attribute__((section("custom_set"), used))
#endif
static const int *const custom_entry = &payload;

int main(void) {
    printf("%x\n", *custom_entry);
    return 0;
}
