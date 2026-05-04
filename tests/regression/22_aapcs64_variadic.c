// Regression: AAPCS64 variadic ABI on -target elf.
//
// Apple's ARM64 ABI passes all variadic args on the stack and uses a
// single-pointer va_list. AAPCS64 (Linux ELF) puts the first 8 GP-passed
// variadic args in x0..x7 and uses a 32-byte va_list struct with a
// register-save area + stack overflow pointer. ncc previously used
// Apple's convention even with -target elf — fine for kernel builds
// (no libc forwarding) but fatal for self-bootstrap on Linux because
// ncc's format() forwards a va_list to glibc's vfprintf, which expects
// the 32-byte struct.
//
// This test exercises a basic va_arg walk (4 args of mixed types) and
// also checks sizeof(va_list) — which is 32 on ELF and a single pointer
// (8) on Apple. We only assert on ELF; the Apple path stays as it was.
//
// Fixed: 2026-05-04. include/stdarg.h gets a struct typedef and macro
// overrides on __ELF__; parse.c adds ND_VA_START_ELF / ND_VA_ARG_ELF;
// codegen_arm64.c saves x0..x7 to a frame area in the variadic prologue
// and emits the runtime conditional read in va_arg.

#include <stdarg.h>

#ifdef __ELF__
_Static_assert(sizeof(va_list) == 32, "AAPCS64 va_list is 32 bytes");
#endif

static int sum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int total = 0;
  for (int i = 0; i < n; i++)
    total += va_arg(ap, int);
  va_end(ap);
  return total;
}

static long sum_mixed(int n, ...) {
  va_list ap;
  va_start(ap, n);
  long total = 0;
  for (int i = 0; i < n; i++) {
    if (i & 1) total += va_arg(ap, long);
    else       total += va_arg(ap, int);
  }
  va_end(ap);
  return total;
}

int main(void) {
  if (sum(4, 1, 2, 3, 4) != 10) return 1;
  if (sum(7, 1, 2, 3, 4, 5, 6, 7) != 28) return 2;
  // 9 args — last one would overflow x0..x7 register save area on ELF
  // (8 GP regs total, 1 named consumes x0, leaves 7 for variadic).
  // The 8th variadic spills to caller's stack overflow.
  if (sum(8, 10, 20, 30, 40, 50, 60, 70, 80) != 360) return 3;
  if (sum_mixed(4, 1, 2L, 3, 4L) != 10) return 4;
  return 0;
}
