// builtins.c - Stub implementations for compiler builtins
// Compile this and link with programs that use builtins

double fabs(double);
float fabsf(float);
long double fabsl(long double);

double __builtin_fabs(double x) { return fabs(x); }
float __builtin_fabsf(float x) { return fabsf(x); }
long double __builtin_fabsl(long double x) { return fabsl(x); }
