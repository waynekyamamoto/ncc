// Regression: __builtin_constant_p walked the AST but missed several
// builtins (clz/ctz/ffs/popcount/parity/clrsb/bswap32/bswap64), so
// ilog2(constant) was incorrectly seen as non-constant inside static_assert.
//
// Also missed: ?: branches with constant cond should fold to the live arm
// before evaluating constness.
//
// Found in Linux btrfs (the remaining 2 of 59 files unblocked here).
// Fixed: 2026-04-27 (commit ad2db17).

extern int foo(void);

#define is_const_p(x) __builtin_constant_p(x)

// __builtin_clz on a constant should be constant.
_Static_assert(is_const_p(__builtin_clz(0xff)),
               "__builtin_clz of constant must be constant");
_Static_assert(is_const_p(__builtin_ctz(0xff)),
               "__builtin_ctz of constant must be constant");
_Static_assert(is_const_p(__builtin_popcount(0xff)),
               "__builtin_popcount of constant must be constant");
_Static_assert(is_const_p(__builtin_bswap32(0xdeadbeef)),
               "__builtin_bswap32 of constant must be constant");

// ?: with constant condition: pick the live branch when checking constness.
_Static_assert(is_const_p(1 ? 42 : foo()),  // foo() is non-const, dead branch
               "?: dead-branch should not block constness");

int main(void) { return 0; }
