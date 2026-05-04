#ifndef __STDARG_H
#define __STDARG_H

// AAPCS64 (Linux ELF) and Apple's ARM64 ABI define va_list incompatibly:
// AAPCS64 uses a 32-byte struct with separate register-save / stack-overflow
// pointers; Apple uses a single pointer (variadic args always pushed to
// stack). We switch on __ELF__ so that on Linux glibc's vfprintf gets the
// va_list shape it expects.
#ifdef __ELF__
typedef struct {
  void *__stack;     // caller's stack overflow start
  void *__gr_top;    // top (end) of GP register save area
  void *__vr_top;    // top (end) of FP register save area
  int   __gr_offs;   // negative byte offset from __gr_top; 0 → use __stack
  int   __vr_offs;   // negative byte offset from __vr_top
} __va_list;
typedef __va_list va_list;
// Override the __builtin_va_list and __gnuc_va_list macros (defined to
// `void *` and `__builtin_va_list` respectively in src/preprocess.c) so
// that downstream system headers like glibc's stdio.h, which do
// `typedef __gnuc_va_list va_list;`, end up resolving va_list to the
// 32-byte struct rather than re-typedefing it to `void *`.
#undef __builtin_va_list
#define __builtin_va_list __va_list
#undef __gnuc_va_list
#define __gnuc_va_list __va_list
#else
typedef __builtin_va_list va_list;
#endif

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_copy(dest, src) __builtin_va_copy(dest, src)

#endif
