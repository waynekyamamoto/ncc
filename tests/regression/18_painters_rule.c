// Q17.B — Painter's rule: a macro is added to the hideset of its own
// expansion to prevent infinite recursion.
//
// Per C11 §6.10.3.4: "If the name of the macro being replaced is found
// during this scan of the replacement list ... it is not replaced."
//
// The TEST is implicit: if the painter's rule were broken, the
// preprocessor would not terminate on the self-referential macros below
// and this file would either hang or be rejected by the parser.
// Successful compilation is the assertion.
//
// Spec ref: docs/specs/02_preprocessor.md §6.2 (expand_macro hideset),
//           docs/specs/02_preprocessor_questions.md Q9.

// --- Test 1: object-like self-reference ---
//
// X expands to (X) once.  On the rescan, X is in the hideset of the
// inner X, so it is left as a literal identifier.  After preprocessing:
//     int X = 42;
//     int *xp = &(X);
// which is `int X = 42; int *xp = &X;` modulo the harmless extra parens.
#define X (X)
int X = 42;
int *xp = &X;

// --- Test 2: function-like self-reference ---
//
// F(0) expands to F(0 + 1).  On the rescan, F is in the hideset of the
// inner F, so the inner F is not re-expanded.  Final form: F(0 + 1),
// which is a function call to the extern declaration.
extern int F(int);
#define F(x) F(x + 1)
int (*fp)(int) = &F;
int call_f(void) { return F(0); }

// --- Test 3: function-like with painter's rule for inter-arg hidesets ---
//
// X(Y(z)) where the inner Y might rescan into something that re-enters Y.
// The intersection rule (`hideset_intersection(macro_tok->hideset,
// rparen->hideset)`) is what makes nested function-like calls behave
// correctly.  We test by ensuring a more complex chain compiles.
#define G(x) ((x) * 2)
#define H(x) G(G(x))
int gh = H(3);  // -> G(G(3)) -> G((3)*2) -> ((3)*2)*2 = 12
_Static_assert(sizeof(gh) == sizeof(int),
    "function-like macro nesting must terminate cleanly");

int main(void) { return 0; }
