// Q4 / Q17.C — Documents that #pragma once is a no-op stub on this
// preprocessor.  Real-world code should rely on `#ifndef ...` include
// guards, not `#pragma once`.  This test confirms the guard pattern
// works — the stub-ness of #pragma once is implicit in the spec
// (§13: "Re-`#include`ing a file with `#pragma once` will include it
// again").  A future test that exercises the negative side could add
// a NN_pragma_once_works.c when/if Q4 is reopened and the stub is
// replaced with real once-tracking.
//
// Spec ref: docs/specs/02_preprocessor.md §12.2 + §13.

#ifndef GUARD_TEST_TOKEN
#define GUARD_TEST_TOKEN 42
#endif

// Re-include the same definition pattern; the guard prevents
// a redefinition error.
#ifndef GUARD_TEST_TOKEN
#define GUARD_TEST_TOKEN 99
#endif

_Static_assert(GUARD_TEST_TOKEN == 42,
    "include-guard pattern must protect against redefinition");

int main(void) { return 0; }
