// Regression: new_add / new_sub did not decay TY_FUNC operands to pointer-
// to-function before classifying them, so a bare function name in pointer
// arithmetic fell through to "invalid operands".
//
// Per C11 6.3.2.1: a function designator used as a value (not as the
// operand of & or sizeof) decays to a pointer to the function. Also,
// function types are sized 0 in ncc, so the elem_size / divisor must
// default to 1 — matching GCC's effective byte-arithmetic behavior.
//
// Found in Linux kernel/bpf/verifier.c:
//   insn->imm = fn->func - __bpf_call_base;
// Fixed: 2026-04-29 (commit 9400497).

typedef unsigned long long u64;

struct proto {
  u64 (*func)(u64, u64, u64, u64, u64);
};

u64 base_fn(u64 a, u64 b, u64 c, u64 d, u64 e);

// (function-pointer) - (bare function): rhs must decay TY_FUNC -> ptr.
long ptr_minus_func(const struct proto *p) {
  return p->func - base_fn;
}

// (bare function) - (function-pointer): symmetric.
long func_minus_ptr(const struct proto *p) {
  return base_fn - p->func;
}

// (bare function) + (int): function decays, byte arithmetic.
unsigned long func_plus_int(void) {
  return (unsigned long)(base_fn + 0);
}

int main(void) { return 0; }
