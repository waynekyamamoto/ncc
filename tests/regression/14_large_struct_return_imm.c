// Regression: function returning a large struct (e.g. 6208 bytes) caused
// gen_funcall to emit `sub sp, sp, #6208` and matching `add sp, sp, #6208`
// for the caller's return-area scratch. ARM64 add/sub-immediate is 12-bit
// (≤4095). Same family as the earlier large-array zero-init bumps.
//
// Found in openssh's kexmlkem768x25519.c (libcrux ML-KEM helpers).
//
// Fix: stage the immediate in x10 and use the register form of sub/add
// when the size exceeds 4095. Also handles padded_stack > 4095 for the
// stack-arg cleanup path.
//
// Fixed: 2026-04-29.

struct big {
  unsigned char data[6208];
};

extern struct big make_big(int seed);

int main(void) {
  // Triggering pattern: caller reserves a 6208-byte scratch slot to receive
  // the returned-by-value struct. Without the fix, the caller's prologue
  // emits `sub sp, sp, #6208` which the assembler accepts but produces
  // wrong code (ARM64 add-imm sign-extends from 12 bits).
  struct big b = make_big(42);
  return (int)b.data[0];
}
