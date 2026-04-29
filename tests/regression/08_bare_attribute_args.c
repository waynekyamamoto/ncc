// Regression: __attribute__((sentinel)) (and similar) without args was rejected.
// ncc had `format / sentinel / alloc_size / cleanup / nonnull` in a branch that
// unconditionally ran skip(tok, "(") — but `sentinel` is commonly used bare.
// Found in git's git-compat-util.h LAST_ARG_MUST_BE_NULL (~194 of git's TUs).
// Fixed: 2026-04-29.

void f(const char *fmt, ...) __attribute__((sentinel));
void g(const char *fmt, ...) __attribute__((sentinel(0)));
void h(const char *fmt) __attribute__((nonnull));
void i(const char *fmt) __attribute__((nonnull(1)));

int main(void) { return 0; }
