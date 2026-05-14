/* Address of a file-scope global stored in another global pointer,
   then called / dereferenced through it. Exercises ADRP+ADD:lo12
   (ELF) and @PAGE/@PAGEOFF (Mach-O) relocations. */

static int global_val = 42;

static int get_val(void) { return global_val; }

typedef int (*fn_t)(void);

static fn_t fp = get_val;
static int *ip = &global_val;

int main(void) {
    if (fp() != 42) return 1;
    if (*ip != 42) return 2;
    if (ip != &global_val) return 3;
    return 0;
}
