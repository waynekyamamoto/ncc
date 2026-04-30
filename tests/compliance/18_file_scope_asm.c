// File-scope __asm("...") emits the directive verbatim into the asm output.
// NetBSD's __strong_alias / __weak_alias macros use this pattern. The asm
// here just declares an alias the linker resolves; the runtime program
// behavior is unaffected.

#include <stdio.h>

int my_target = 42;

#ifdef __APPLE__
// Mach-O .globl directive uses leading underscore.
__asm(".globl _my_target_alias\n_my_target_alias = _my_target");
#else
__asm(".globl my_target_alias\nmy_target_alias = my_target");
#endif

extern int my_target_alias;

int main(void) {
    printf("%d\n", my_target_alias);
    return 0;
}
