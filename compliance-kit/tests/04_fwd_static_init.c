/* SPEC §3.4 — a file-scope initializer may reference a static
   function whose body is defined later in the same translation unit. */

struct ops { int (*f)(void); };

static int impl(void);

static struct ops o = { impl };

static int impl(void) { return 42; }

int main(void) {
    return o.f() != 42;
}
