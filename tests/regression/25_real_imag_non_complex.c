// Q11.C / spec §9 (ND_REAL/ND_IMAG dispatch) + §10 — GCC convention:
//   __real__(x) on a non-complex x is identity (returns x).
//   __imag__(x) on a non-complex x is zero, typed as x.
//
// Spec ref: docs/specs/03_type.md §9 dispatch table + §10
//   "Known divergences" entry on __real__/__imag__.

_Static_assert(__real__(3) == 3,
    "__real__ on a non-complex int is identity");
_Static_assert(__imag__(3) == 0,
    "__imag__ on a non-complex int is zero");

_Static_assert(__real__(42L) == 42L,
    "__real__ on a long is identity");
_Static_assert(__imag__(42L) == 0L,
    "__imag__ on a long is zero");

// Type preservation: the result has the operand's type.
_Static_assert(_Generic(__real__(3), int: 1, default: 0) == 1,
    "__real__(int) has type int");
_Static_assert(_Generic(__imag__(3), int: 1, default: 0) == 1,
    "__imag__(int) has type int");
_Static_assert(_Generic(__real__(3L), long: 1, default: 0) == 1,
    "__real__(long) has type long");

int main(void) { return 0; }
