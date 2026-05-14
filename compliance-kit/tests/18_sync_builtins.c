/* SPEC §4.2 — `__sync_*` builtins lowered inline. Exercises
   synchronize, CAS (match and mismatch), test-and-set, and
   lock-release. */

static int x = 0;

int main(void) {
    __sync_synchronize();

    /* CAS: succeed on matching old value. */
    if (__sync_bool_compare_and_swap(&x, 0, 1) != 1) return 1;
    if (x != 1) return 2;

    /* CAS: fail when old doesn't match. */
    if (__sync_bool_compare_and_swap(&x, 0, 99) != 0) return 3;
    if (x != 1) return 4;

    /* test-and-set: returns prior value, stores new. */
    int prev = __sync_lock_test_and_set(&x, 5);
    if (prev != 1) return 5;
    if (x != 5) return 6;

    /* lock-release: stores 0 with release semantics. */
    __sync_lock_release(&x);
    if (x != 0) return 7;

    return 0;
}
