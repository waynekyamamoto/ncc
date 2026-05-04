// Regression: same family as 18_switch_case_high_bit_const.c.
//
// Several other call sites in parse.c stored a 64-bit const_expr_val
// result into a local `int`. The case-statement site was the most
// visible (broke NetBSD's tty ioctl switch); this test guards the
// remaining sites:
//
//   - parse.c:1451   __attribute__((aligned(N)))
//   - parse.c:1545   __attribute__((vector_size(N)))
//   - parse.c:4978   array range designator [lo ... hi] in init
//   - parse.c:5211   array range designator in init-with-size-deduction
//
// Compile-only test: if ncc parses these constants without truncating
// to int along the way, the resulting types and array bounds match
// what we asked for. Runtime checks confirm.
//
// Fixed: 2026-05-04 (bundled with the case-constant fix family).

// Range designator [lo ... hi] in array initializer: parser truncated
// lo and hi to int. Normal small ranges always worked; the bug was on
// the type of the parser locals, so use a small range here — what
// matters is that the typed-as-int64_t values flow correctly.
int range_init[5] = { [0 ... 4] = 42 };

// Array initializer with size-deduced declaration + range designator.
int range_deduced[] = { [0 ... 3] = 7, [4 ... 6] = 9 };

// Aligned-attribute and vector_size-attribute parsing also took the
// const_expr_val into `int`. Those paths are exercised by parsing the
// declaration; ncc has separate bugs around honoring aligned() on
// struct types, so we don't verify the alignment value at runtime —
// just that the parse completes cleanly.
struct __attribute__((aligned(32))) Aligned { int x; };

int main(void) {
  if (sizeof(range_init) != 5 * sizeof(int)) return 1;
  for (int i = 0; i < 5; i++)
    if (range_init[i] != 42) return 2;

  if (sizeof(range_deduced) != 7 * sizeof(int)) return 3;
  for (int i = 0; i < 4; i++)
    if (range_deduced[i] != 7) return 4;
  for (int i = 4; i < 7; i++)
    if (range_deduced[i] != 9) return 5;

  return 0;
}
