// Regression: ncc was missing Darwin deployment-target predefines, and
// __has_include() was hardcoded to 0 instead of probing the include path.
//
// Found in redis (6+ files via __has_include in <malloc/_platform.h>)
// and git (compat/regcomp_enhanced.c relying on REG_ENHANCED gating).
//
// Fixed: 2026-04-29 — predefined __MAC_OS_X_VERSION_{MIN_REQUIRED,MAX_ALLOWED},
// __ENVIRONMENT_{MAC_OS_X_VERSION_MIN_REQUIRED,OS_VERSION_MIN_REQUIRED}__,
// and made __has_include() probe the search path.

#if !defined(__MAC_OS_X_VERSION_MIN_REQUIRED)
#error "__MAC_OS_X_VERSION_MIN_REQUIRED not predefined"
#endif

#if !defined(__MAC_OS_X_VERSION_MAX_ALLOWED)
#error "__MAC_OS_X_VERSION_MAX_ALLOWED not predefined"
#endif

#if !defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
#error "__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ not predefined"
#endif

// stdio.h ships with every C compiler — __has_include must say 1.
#if !__has_include(<stdio.h>)
#error "__has_include(<stdio.h>) returned 0; should be 1"
#endif

// Bogus path must say 0.
#if __has_include(<this_file_does_not_exist_anywhere.h>)
#error "__has_include of non-existent header returned 1"
#endif

int main(void) { return 0; }
