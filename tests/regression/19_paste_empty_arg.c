// Q17.D — Token paste (`##`) with a *literally empty* argument is
// skipped: the placemarker token absorbs into the non-empty side.
//
// Per C11 §6.10.3.3 paragraph 2: "If either operand is a placemarker
// pp-token, the result is the other operand."  This is invoked by
// `subst` when an argument's raw token list is empty (not when an
// argument macro-expands to empty — `##` operates on raw tokens, not
// expanded ones, per §6.10.3.1).
//
// Spec ref: docs/specs/02_preprocessor.md §7.3 (empty-arg paste).
//
// IMPORTANT distinction from a near-miss test design: `CONCAT(foo,
// EMPTY)` where `EMPTY` is `#define EMPTY` produces `fooEMPTY` (paste
// of literal tokens, no pre-expansion).  Only `CONCAT(foo,)` (with no
// token between the comma and the close paren) triggers the
// placemarker rule.

#define CONCAT(a, b) a ## b

// Literally-empty RHS argument: paste skipped, result is `foo`.
int CONCAT(foo,) = 1;
_Static_assert(sizeof(foo) == sizeof(int),
    "paste with literally-empty RHS yields LHS");

// Literally-empty LHS argument: paste skipped, result is `bar`.
int CONCAT(,bar) = 2;
_Static_assert(sizeof(bar) == sizeof(int),
    "paste with literally-empty LHS yields RHS");

// GNU extension: `, ## __VA_ARGS__` deletes the comma when __VA_ARGS__
// is empty.  This is a separate rule from the §6.10.3.3 placemarker
// rule, but related and worth covering.
#define LOG_GNU(fmt, ...) (sizeof(fmt) , ## __VA_ARGS__)
// LOG_GNU("hi") with empty __VA_ARGS__ -> (sizeof("hi")) -- no trailing comma
_Static_assert(LOG_GNU("hi") == 3, "GNU comma-paste deletes comma when __VA_ARGS__ empty");
// LOG_GNU("hi", 7) -> (sizeof("hi") , 7) -> 7 (comma operator)
_Static_assert(LOG_GNU("hi", 7) == 7, "GNU comma-paste preserves comma when __VA_ARGS__ non-empty");

int main(void) { return foo + bar; }
