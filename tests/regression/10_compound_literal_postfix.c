// Regression: postfix() returned directly after parsing a compound literal,
// so `(T){...}[i]`, `(T){...}.field`, `(T){...}->field`, etc. failed with
// "expected ';'".
//
// Found in musl: fmemopen.c uses `(int [4]){...}[whence]`.
//
// Fix: fall through to the postfix-suffix loop after building the compound-
// literal node.
//
// Fixed: 2026-04-29.

struct point { int x, y; };

int main(void) {
  // Subscript after compound literal.
  int a = (int[4]){10, 20, 30, 40}[2];
  if (a != 30) return 1;

  // Member access after compound literal.
  int b = (struct point){.x = 7, .y = 8}.y;
  if (b != 8) return 2;

  // Member-via-pointer would need a pointer compound literal; subscript on
  // an array compound literal also exercises ND_DEREF + ND_ADD.
  return 0;
}
