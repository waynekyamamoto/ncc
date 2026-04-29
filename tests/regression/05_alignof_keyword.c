// Regression: ncc only recognized __alignof__ / _Alignof, not __alignof.
// Linux kernel and Wireguard use the bare __alignof spelling.
//
// Found in drivers/net/wireguard.
// Fixed: 2026-04-27 (commit 55a4e7f).

typedef unsigned long long u64;

// All three spellings must work and give the same answer.
_Static_assert(__alignof(u64) == __alignof__(u64),
               "__alignof must equal __alignof__");
_Static_assert(__alignof(u64) == _Alignof(u64),
               "__alignof must equal _Alignof");

// In a real-world alignment attribute.
struct s {
  char x;
  u64 y __attribute__((aligned(__alignof(u64))));
};

int main(void) { return 0; }
