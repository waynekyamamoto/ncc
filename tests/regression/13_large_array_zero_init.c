// Regression: ND_MEMZERO emitted `add x1, x1, #zoff` with zoff up to 32760
// to bump the base pointer between str-x writes, but ARM64 add-immediate
// is 12-bit (≤4095). Assembler accepted but produced wrong code; for the
// stp prologue path the assembler errored outright.
//
// Found in redis/src/db.c (`size_t slot_sizes[CLUSTER_SLOTS] = {0};` is
// a 131072-byte stack array) and musl vfprintf.c (large fixed array +
// VLA in same function triggers stp prologue zero-init).
//
// Fix: cap the periodic bump at 4088 (largest 8-aligned value ≤ 4095) so
// the `add` always fits 12-bit immediate. For the VLA prologue stp path,
// thread through a base register (x9 = sp) and bump it within stp's
// signed-imm range [-512, 504].
//
// Fixed: 2026-04-29.

// 16 KiB stack array, fully zero-initialized. Triggers many str-x bumps.
int big_array_zero_init(void) {
  long buf[2048] = {0};       // 16384 bytes — will hit the bump path
  return (int)buf[0];
}

int main(void) { return big_array_zero_init(); }
