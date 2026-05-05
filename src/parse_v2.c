// parse_v2.c — Phase 4 spec-derived parser (skeleton).
//
// Implementation will be derived from docs/specs/04_parse.md and the
// four sub-chunk specs (04a_decl, 04b_expr, 04c_stmt, 04d_init) under
// strict no-peek discipline (no consultation of src/parse.c during
// impl).  Built into the alternate `ncc-v2` binary; validated against
// the canonical `ncc` via bootstrap fixed-point + torture + real
// programs before swap-in.
//
// This skeleton provides the public symbols declared in cc.h so the
// dual-build links cleanly.  Real implementations land in chunks per
// the Phase 4 plan (Q3.A: single big-bang swap-in).
//
// Public surface (cc.h §parse.c):
//   Obj  *parse(Token *tok);
//   Node *new_cast(Node *expr, Type *ty);
//   int64_t eval_node(Node *node);
//   bool  try_eval_node(Node *node, int64_t *out);
//   extern int label_cnt;
//   extern int gvar_cnt;

#include "cc.h"

// Globals consumed by codegen_arm64.c and main.c.
int label_cnt;
int gvar_cnt;

// Stubs — chunked impls will replace these per docs/specs/04*.md.

Obj *parse(Token *tok) {
  (void)tok;
  return NULL;
}

Node *new_cast(Node *expr, Type *ty) {
  (void)ty;
  return expr;
}

int64_t eval_node(Node *node) {
  (void)node;
  return 0;
}

bool try_eval_node(Node *node, int64_t *out) {
  (void)node;
  (void)out;
  return false;
}
