#ifndef __LIMITS_H
#define __LIMITS_H

// Glibc's /usr/include/limits.h chains to a "compiler-provided" limits.h via
// `#include_next <limits.h>`, gated on `_GCC_LIMITS_H_` not being defined.
// Defining it here mirrors what gcc's bundled limits.h does, breaking what
// would otherwise be a chain that goes off the end of the include path.
#define _GCC_LIMITS_H_

#define CHAR_BIT 8

#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255

#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX

#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535

#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295U

#define LONG_MIN (-9223372036854775807L - 1)
#define LONG_MAX 9223372036854775807L
#define ULONG_MAX 18446744073709551615UL

#define LLONG_MIN (-9223372036854775807LL - 1)
#define LLONG_MAX 9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL

#define MB_LEN_MAX 6

// Include the system limits.h for additional definitions like PATH_MAX
#include_next <limits.h>

// Re-assert our correctly-typed definitions (system headers may use
// untyped hex literals like 0xffffffff instead of 4294967295U)
#undef UINT_MAX
#define UINT_MAX 4294967295U
#undef ULONG_MAX
#define ULONG_MAX 18446744073709551615UL
#undef ULLONG_MAX
#define ULLONG_MAX 18446744073709551615ULL

#endif
