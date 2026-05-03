// Q17.A — Stringize must NOT rescan its argument.
// Per C11 §6.10.3.2: "If the argument is a parameter, then the corresponding
// argument's preprocessing tokens are joined ... before substitution and
// without further macro expansion."
//
// In other words, `#x` operates on the RAW argument tokens (`arg->tok`),
// never on the pre-expanded form (`arg->expanded`).
//
// Counter-test: regular argument substitution DOES rescan, so STR_RESCANNED
// shows the contrast.
//
// Spec ref: docs/specs/02_preprocessor.md §6.4 (subst) + §7.1 (#),
//           docs/specs/02_preprocessor_questions.md Q8.

#define FOO 42
#define STR(x) #x
#define STR_RESCANNED(x) STR(x)

// STR(FOO) must produce "FOO" (4 bytes including NUL terminator),
// not "42" (3 bytes).  If stringize rescanned, it would be 3.
_Static_assert(sizeof(STR(FOO)) == 4,
    "stringize must NOT rescan: STR(FOO) -> \"FOO\" (sizeof 4), not \"42\" (sizeof 3)");

// Regular argument substitution DOES rescan, so STR_RESCANNED(FOO)
// expands FOO to 42 first, then stringizes -> "42" (sizeof 3).
_Static_assert(sizeof(STR_RESCANNED(FOO)) == 3,
    "regular arg substitution DOES rescan: STR_RESCANNED(FOO) -> \"42\" (sizeof 3)");

int main(void) { return 0; }
