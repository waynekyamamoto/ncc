// Regression: switch-case constants with bit 31 set were sign-extended
// to 64 bits during codegen, so the cmp against an unsigned long
// switch expression never matched.
//
// Path: parse.c case-statement parse stored the const_expr_val (int64_t)
// into a local `int begin`, truncating to 32-bit. Bit 31 set (e.g.
// ioctl number 0x80047410) became negative int. Assigning to
// node->begin (long) sign-extended to 0xFFFFFFFF80047410. codegen_arm64
// then emitted a 4-movk sequence with the upper 32 bits set, and the
// runtime cmp against the zero-extended u_long expression never matched
// — every such case fell through to default.
//
// Surfaced as: NetBSD/aarch64 ncc-built kernel returned ENOSYS for every
// tty ioctl whose number had the IOW/IOR direction flag set, because
// ttioctl()'s big switch in kern/tty.c stopped matching those cases.
// Symptom: "ttyflags: TIOCSFLAGS on /dev/console: Function not
// implemented" + getty respawn loop, no login.
//
// Fixed: 2026-05-04. parse.c:4163 + 4168 changed `int begin/end` to
// `int64_t begin/end`.

_Static_assert(sizeof(unsigned long) == 8, "expecting 64-bit long");

int test(unsigned long cmd) {
  switch (cmd) {
  case 0x80047410UL:  // 32-bit value with bit 31 set; the bug case.
    return 1;
  case 0x80048004UL:
    return 2;
  default:
    return 0;
  }
}

int main(void) {
  // Build the value at runtime so the switch isn't constant-folded away.
  unsigned long a = 0x80047410UL;
  unsigned long b = 0x80048004UL;
  unsigned long c = 0x12345678UL;
  if (test(a) != 1) return 1;
  if (test(b) != 2) return 2;
  if (test(c) != 0) return 3;
  return 0;
}
