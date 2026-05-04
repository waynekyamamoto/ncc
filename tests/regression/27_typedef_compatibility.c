// Q11.E / spec §6 (is_compatible) — typedef transparency.
//
// is_compatible walks the `origin` chain set by copy_type, so a
// typedef'd type is compatible with its underlying type.  This
// test exercises:
//   - Direct typedef compatibility (typedef'd ptr assignable to
//     untyped equivalent).
//   - Multi-level typedef chain.
//   - typedef-of-pointer-of-typedef interaction.
//
// Spec ref: docs/specs/03_type.md §6 (typedef transparency via
//   origin chain).

typedef int my_int;
typedef my_int my_int_alias;       // typedef of typedef
typedef int *my_int_ptr;
typedef my_int *my_int_alias_ptr;  // typedef-pointer of typedef

// Simple: my_int is compatible with int.  &x has type my_int *,
// assignable to int * at file scope (the address-of-extern is a
// constant expression).
my_int x = 5;
int *p1 = &x;

// Multi-level chain (typedef-of-typedef).
my_int_alias y = 7;
int *p2 = &y;

// typedef-pointer (pointer-to-typedef): &x is constant, so OK at
// file scope.  The variable-to-variable assignment (my_int_ptr ->
// int *) requires a constant expression at file scope, so we do
// it inside main below.
my_int_ptr fp_to_x = &x;

// Function compatibility through typedef.
typedef int my_func_t(int);
my_func_t my_increment;
int my_increment(int v) { return v + 1; }

// Function pointer typedef.
typedef int (*my_fp_t)(int);
my_fp_t fp = my_increment;

int main(void) {
    // Variable-to-variable typedef-pointer assignment — exercises
    // is_compatible's typedef transparency on a runtime
    // assignment, not just a constant initializer.
    int *p4 = fp_to_x;
    (void)p4;

    return fp(41) - 42;   // returns 0
}
