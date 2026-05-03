// Q17.H — `__VA_OPT__(content)` substitutes `content` if the variadic
// argument is non-empty, nothing otherwise.
//
// Per C23 §6.10.5.2 (and GCC/clang extension predating C23): the canonical
// use case is the comma in variadic logging macros:
//
//     #define LOG(fmt, ...) printf(fmt __VA_OPT__(,) __VA_ARGS__)
//
// LOG("hello\n")     ->  printf("hello\n"  )      (no comma after fmt)
// LOG("%d\n", 42)    ->  printf("%d\n" , 42)      (comma included)
//
// Spec ref: docs/specs/02_preprocessor.md §6.4 (subst — __VA_OPT__),
//           docs/specs/02_preprocessor_questions.md Q17.H.

// --- Test: bare __VA_OPT__ result usable in constant expression ---
//
// __VA_OPT__(1) + 0:
//   variadic empty:    "" + 0  -> can't parse; need a non-empty fallback.
// Use:
//   __VA_OPT__(1) 0
//   variadic empty:    0       -> integer constant 0
//   variadic non-empty: 1 0    -> two adjacent tokens (syntax error if unguarded)
// To make both branches well-formed:
//   (__VA_OPT__(1+) 0)
//   variadic empty:    (0)     -> 0
//   variadic non-empty:(1+ 0)  -> 1
#define HAS_ARG(...) (__VA_OPT__(1+) 0)

_Static_assert(HAS_ARG() == 0,
    "__VA_OPT__ with empty variadic produces nothing");
_Static_assert(HAS_ARG(x) == 1,
    "__VA_OPT__ with non-empty variadic produces its content");
_Static_assert(HAS_ARG(x, y, z) == 1,
    "__VA_OPT__ with multi-token variadic produces its content");

int main(void) { return 0; }
