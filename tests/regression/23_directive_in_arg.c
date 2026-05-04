// Q17.E — Conditional directives inside macro argument lists.
//
// Per spec §6.3 + §8.5, when a #if / #ifdef / #ifndef etc. appears
// inside a macro argument list, read_macro_args delegates to
// handle_pp_directive_in_arg which shares the cond_incl stack with
// the main preprocessor loop.  This test exercises a pattern from
// Linux's lib/decompress_inflate.c (a ternary expression where one
// of the alternatives is gated on a #define).
//
// Spec ref: docs/specs/02_preprocessor.md §6.3 + §8.5.

#define ID(x) x
#define USE_INNER 1
#define USE_INNER_OFF 0

enum {
    CHOSEN = ID(
#if USE_INNER
        100
#else
        200
#endif
    ),
    OTHER = ID(
#if USE_INNER_OFF
        111
#else
        222
#endif
    ),
};

_Static_assert(CHOSEN == 100,
    "in-arg #if must select THEN branch when USE_INNER is true");
_Static_assert(OTHER == 222,
    "in-arg #if must select ELSE branch when USE_INNER_OFF is false");

int main(void) { return 0; }
