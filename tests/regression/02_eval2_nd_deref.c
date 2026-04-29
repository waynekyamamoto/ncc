// Regression: eval2 was missing ND_DEREF case, breaking offsetof of nested
// array elements like offsetof(s, bps[0][1]).
//
// Path: ND_ADDR -> eval_rval(ND_DEREF) -> eval2(ND_ADD) -> eval2(ND_DEREF inner)
// hit default and emitted "not a compile-time constant".
//
// Found in Linux kernel block/blk-throttle.c.
// Fixed: 2026-04-27 (commit ad2db17).

#include <stddef.h>

struct s {
  int bps[4][2];
};

int main(void) {
  // Trigger the path: offsetof through a nested array.
  return offsetof(struct s, bps[0][1]) == sizeof(int) ? 0 : 1;
}
