// Regression: __gnuc_va_list typedef recognition.
//
// One of two blockers for ncc self-bootstrapping on Linux: Ubuntu
// glibc's /usr/include/stdio.h:52 does
//
//     typedef __gnuc_va_list va_list;
//
// where __gnuc_va_list is supposed to be defined by the compiler's
// bundled <stddef.h> as `typedef __builtin_va_list __gnuc_va_list;`.
// gcc and clang recognize it. ncc didn't, so any TU that ended up
// pulling in glibc's stdio.h failed to parse the typedef.
//
// Fix: define_macro("__gnuc_va_list", "__builtin_va_list") next to the
// existing __builtin_va_list line in src/preprocess.c, mirroring how
// gcc itself does it.
//
// Fixed: 2026-05-04.

typedef __gnuc_va_list va_list;

int sink(va_list ap) { (void)ap; return 0; }

int main(void) {
  va_list ap = 0;
  return sink(ap);
}
