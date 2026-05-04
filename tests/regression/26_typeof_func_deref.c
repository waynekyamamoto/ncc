// Q11.D / spec §9 (ND_DEREF dispatch) + §10 — GCC typeof extension:
//   `*(func)` has the function type, NOT the return type.  This
//   supports patterns like `typeof(*func) *fnptr = func`.
//
// Spec ref: docs/specs/03_type.md §9 (ND_DEREF) + §10 entry on
//   "*(func) returns the function type".

extern int my_func(int x);

// typeof(*my_func) should be `int(int)`, the function type.
// typeof(*my_func) * should therefore be `int(*)(int)`, a
// function-pointer type.  Assigning &my_func to it must
// type-check (the array-of-pointer / function-decay rule).
typedef typeof(*my_func) func_t;
func_t *fp = &my_func;

// Actually use the pointer to confirm it's callable.
int call_via_typeof(int x) {
    return fp(x);
}

// Different syntactic form — typeof inside a more complex
// declaration.
int call_inline(int x) {
    typeof(*my_func) *q = my_func;
    return q(x);
}

int main(void) { return 0; }
