// Regression: array_dimensions' is-constant walker only descends node->lhs
// and node->rhs, so an ND_STMT_EXPR (`({ ... })`) was treated as constant
// and routed to eval(), which has no ND_STMT_EXPR case and aborts with
// "not a compile-time constant".
//
// The kernel's max(x,y) chain expands via __builtin_choose_expr to a
// stmt-expr (__cmp_once) when either argument isn't a constant, and
// drivers/md/dm-integrity.c uses max(...) as a stack-array bound.
//
// Fix: in src/parse.c array_dimensions, treat ND_STMT_EXPR as non-constant
// so the array becomes a VLA instead of crashing eval().
//
// Found in Linux drivers/md/dm-integrity.c.
// Fixed: 2026-04-29.

extern int side_effect(int);

int main(void) {
  // A statement expression as the array size — must take the VLA path,
  // not crash with "not a compile-time constant".
  char buf[({ int t = side_effect(7); t > 0 ? t : 1; })];
  return (int)sizeof(buf);
}
