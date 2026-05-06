// parse_v2.c — Phase 4 spec-derived parser.
//
// Derived from docs/specs/04_parse.md and the four sub-chunk specs
// (04a_decl, 04b_expr, 04c_stmt, 04d_init) under strict no-peek
// discipline (no consultation of src/parse.c during impl).  Built into
// the alternate `ncc-v2` binary; validated against canonical `ncc` via
// bootstrap fixed-point + torture + real programs before swap-in.
//
// Public surface (cc.h §parse.c):
//   Obj  *parse(Token *tok);
//   Node *new_cast(Node *expr, Type *ty);
//   int64_t eval_node(Node *node);
//   bool  try_eval_node(Node *node, int64_t *out);
//   extern int label_cnt;
//   extern int gvar_cnt;
//
// Status: SPINE.  This file currently contains:
//   - File-scope state (globals, scope).
//   - Internal types (VarScope, TagScope, Scope, VarAttr) per
//     04c_stmt.md §A.1 and 04a_decl.md §B.
//   - Scope mechanics (enter/leave/find/push) per 04c §A.
//   - is_typename per 04_parse.md §5.
//   - parse() top-level driver per 04a §I.
//   - Stubs for declspec, declarator, parse_typedef, function,
//     global_variable, attribute_list, static_assert path.  Each
//     errors out via error_tok when reached.
//
// Real impls land per the Phase 4 plan (Q3.A: single big-bang swap-in
// after bootstrap fixed-point + corpus pass).

#include "cc.h"

//
// Internal types (parser-private).
//

// VarScope: ordinary-namespace binding.  One of {var, type_def,
// enum_ty + enum_val} is set per entry (mutually exclusive per
// C11 §6.2.3).
typedef struct VarScope VarScope;
struct VarScope {
  VarScope *next;
  char *name;
  Obj *var;        // ordinary variable / function
  Type *type_def;  // typedef binding
  Type *enum_ty;   // enum constant: which enum
  int enum_val;    // enum constant: value
};

// TagScope: tag-namespace binding (struct/union/enum tags).
typedef struct TagScope TagScope;
struct TagScope {
  TagScope *next;
  char *name;
  Type *ty;
};

// Scope: a single lexical scope.  Chain via `next` outward.
struct Scope {
  Scope *next;
  VarScope *vars;  // ordinary namespace
  TagScope *tags;  // tag namespace
};

// VarAttr: storage-class & alignment attributes accumulated by
// declspec (04a_decl.md §B.3, §B.6, §G).  mode_kind / vector_size
// are populated by attribute_list (§G) and applied post-loop (§B.12).
typedef struct {
  bool is_typedef;
  bool is_static;
  bool is_extern;
  bool is_inline;
  bool is_noreturn;
  bool is_tls;
  int align;
  int mode_kind;     // 0 / 1 / 2 / 4 / 8 — set by __attribute__((mode(X)))
  int vector_size;   // bytes — set by __attribute__((vector_size(N)))
} VarAttr;

//
// Globals consumed by codegen_arm64.c and main.c.
//
int label_cnt;
int gvar_cnt;

//
// File-scope tables.
//
// These were originally function-static, but parse_v2.c can't yet
// compile itself when arrays-of-struct appear inside function bodies
// (array-of-struct gvar init is a follow-up).  Lifting to file scope
// is a clean workaround that doesn't change semantics.
//

typedef struct CompoundOp { const char *op; NodeKind kind; } CompoundOp;
static const CompoundOp compound_op_table[] = {
  {"+=",  ND_ADD},
  {"-=",  ND_SUB},
  {"*=",  ND_MUL},
  {"/=",  ND_DIV},
  {"%=",  ND_MOD},
  {"&=",  ND_BITAND},
  {"|=",  ND_BITOR},
  {"^=",  ND_BITXOR},
  {"<<=", ND_SHL},
  {">>=", ND_SHR},
};

typedef struct UnaryBuiltin { const char *name; NodeKind kind; bool is64; } UnaryBuiltin;
static const UnaryBuiltin unary_builtin_table[] = {
  {"__builtin_clz",         ND_BUILTIN_CLZ,        false},
  {"__builtin_clzl",        ND_BUILTIN_CLZ,        true },
  {"__builtin_clzll",       ND_BUILTIN_CLZ,        true },
  {"__builtin_ctz",         ND_BUILTIN_CTZ,        false},
  {"__builtin_ctzl",        ND_BUILTIN_CTZ,        true },
  {"__builtin_ctzll",       ND_BUILTIN_CTZ,        true },
  {"__builtin_ffs",         ND_BUILTIN_FFS,        false},
  {"__builtin_ffsl",        ND_BUILTIN_FFS,        true },
  {"__builtin_ffsll",       ND_BUILTIN_FFS,        true },
  {"__builtin_popcount",    ND_BUILTIN_POPCOUNT,   false},
  {"__builtin_popcountl",   ND_BUILTIN_POPCOUNT,   true },
  {"__builtin_popcountll",  ND_BUILTIN_POPCOUNT,   true },
  {"__builtin_parity",      ND_BUILTIN_PARITY,     false},
  {"__builtin_parityl",     ND_BUILTIN_PARITY,     true },
  {"__builtin_parityll",    ND_BUILTIN_PARITY,     true },
  {"__builtin_clrsb",       ND_BUILTIN_CLRSB,      false},
  {"__builtin_clrsbl",      ND_BUILTIN_CLRSB,      true },
  {"__builtin_clrsbll",     ND_BUILTIN_CLRSB,      true },
  {"__builtin_bswap32",     ND_BUILTIN_BSWAP32,    false},
  {"__builtin_bswap64",     ND_BUILTIN_BSWAP64,    true },
};

//
// File-scope parser state.
//
static Obj *globals;
static Scope *scope;

// Per-function state.  Saved on the C stack across nested function
// definitions in `function` (04c §F.1).
static Obj  *current_fn;
static Obj  *locals;
static Node *gotos;
static Node *labels;
static char *brk_label;
static char *cont_label;
static Node *current_switch;

//
// Forward declarations.
//
static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr);
static Node *new_var_node(Obj *var, Token *tok);
static Node *to_assign(Node *binary);
static Node *new_inc_dec(Node *node, Token *tok, int addend);
static Node *new_add(Node *lhs, Node *rhs, Token *tok);
static Node *new_sub(Node *lhs, Node *rhs, Token *tok);
static Obj *new_anon_gvar(Type *ty);
static int64_t const_expr_val(Token **rest, Token *tok);
static bool try_eval_double_v2(Node *n, double *out);
static Token *gvar_subinit_recursive(Token *tok, Type *ty, char *buf,
                                      long base_off, long buf_sz,
                                      Relocation **rh_tail);
static Node *lvar_init_at_offset(Token **rest, Token *tok, Type *ty,
                                  Obj *var, long base_off, Node *chain,
                                  Token *eq);
static Type *typename_(Token **rest, Token *tok);
static Node *expr(Token **rest, Token *tok);
static Node *assign(Token **rest, Token *tok);
static Node *cond_expr(Token **rest, Token *tok);
static Node *logor(Token **rest, Token *tok);
static Node *logand(Token **rest, Token *tok);
static Node *bitor_(Token **rest, Token *tok);
static Node *bitxor(Token **rest, Token *tok);
static Node *bitand_(Token **rest, Token *tok);
static Node *equality(Token **rest, Token *tok);
static Node *relational(Token **rest, Token *tok);
static Node *shift(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok);
static Node *mul(Token **rest, Token *tok);
static Node *cast(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *postfix(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);
static Node *stmt(Token **rest, Token *tok);
static Node *compound_stmt(Token **rest, Token *tok);
static Type *declspec(Token **rest, Token *tok, VarAttr *attr);
static Type *declarator(Token **rest, Token *tok, Type *ty);
static Type *type_suffix(Token **rest, Token *tok, Type *ty);
static Type *func_params(Token **rest, Token *tok, Type *ty);
static Type *array_dimensions(Token **rest, Token *tok, Type *ty);
static Type *pointers(Token **rest, Token *tok, Type *ty);
static Token *parse_typedef(Token *tok, Type *basety);
static Token *function(Token *tok, Type *basety, Type *ty, VarAttr *attr);
static Token *function_declaration(Token *tok, Type *basety, Type *ty, VarAttr *attr);
static Token *global_variable(Token *tok, Type *basety, Type *first_ty, VarAttr *attr);
static Token *skip_attribute(Token *tok);
static Token *skip_static_assert(Token *tok);

//
// Scope mechanics (04c_stmt.md §A).
//

static void enter_scope(void) {
  Scope *sc = calloc_checked(1, sizeof(Scope));
  sc->next = scope;
  scope = sc;
}

static void leave_scope(void) {
  scope = scope->next;
}

static VarScope *find_var(Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->next)
    for (VarScope *vs = sc->vars; vs; vs = vs->next)
      if (vs->name && tok->len == (int)strlen(vs->name) &&
          !strncmp(tok->loc, vs->name, tok->len))
        return vs;
  return NULL;
}

static VarScope *push_scope(char *name) {
  VarScope *vs = calloc_checked(1, sizeof(VarScope));
  vs->name = name;
  vs->next = scope->vars;
  scope->vars = vs;
  return vs;
}

static TagScope *find_tag(Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->next)
    for (TagScope *ts = sc->tags; ts; ts = ts->next)
      if (ts->name && tok->len == (int)strlen(ts->name) &&
          !strncmp(tok->loc, ts->name, tok->len))
        return ts;
  return NULL;
}

static void push_tag_scope(Token *tok, Type *ty) {
  TagScope *ts = calloc_checked(1, sizeof(TagScope));
  ts->name = strndup_checked(tok->loc, tok->len);
  ts->ty = ty;
  ts->next = scope->tags;
  scope->tags = ts;
}

//
// Node construction helpers (04b_expr.md §A).
//

static Node *new_node(NodeKind kind, Token *tok) {
  Node *node = calloc_checked(1, sizeof(Node));
  node->kind = kind;
  node->tok = tok;
  return node;
}

static Node *new_unary(NodeKind kind, Node *expr_, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = expr_;
  return node;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

static Node *new_num(int64_t val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  node->ty = ty_int;
  return node;
}

// 64-bit num — used for pointer-arithmetic scale factors so the
// generated MUL stays in 64-bit registers (otherwise high bits get
// truncated and pointer arithmetic on stack addresses goes wild).
static Node *new_long(int64_t val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  node->ty = ty_long;
  return node;
}

// Spec 04c §A.5 prescribes `.L..%d`, but Mach-O treats `.L`-prefixed
// labels as external — only labels starting with capital `L` are
// local in Mach-O.  We use `Ltmp_%d` to match the canonical ncc and
// the assembler's expectations.  Documented divergence from spec.
static char *new_unique_name(void) {
  return format("Ltmp_%d", label_cnt++);
}

//
// Variable creation helpers (04a_decl.md §J).
//

static Obj *new_var(char *name, Type *ty) {
  Obj *var = calloc_checked(1, sizeof(Obj));
  var->name = name;
  var->ty = ty;
  var->align = ty->align;
  push_scope(name)->var = var;
  return var;
}

static Obj *new_gvar(char *name, Type *ty) {
  Obj *var = new_var(name, ty);
  var->next = globals;
  var->is_definition = true;
  var->is_static = false;
  globals = var;
  return var;
}

static Obj *new_lvar(char *name, Type *ty) {
  Obj *var = new_var(name, ty);
  var->is_local = true;
  var->next = locals;
  locals = var;
  return var;
}

//
// __asm__ label consumer (04a_decl.md §G.1).  Label content is
// discarded — main does not yet honor explicit asm labels in symbol
// emission (a known divergence-log item).
//

static Token *skip_asm_label(Token *tok) {
  if (!equal(tok, "asm") && !equal(tok, "__asm") && !equal(tok, "__asm__"))
    return tok;
  tok = tok->next;
  tok = skip(tok, "(");
  int depth = 1;
  while (depth > 0 && tok->kind != TK_EOF) {
    if (equal(tok, "(")) depth++;
    else if (equal(tok, ")")) { depth--; if (depth == 0) break; }
    tok = tok->next;
  }
  tok = skip(tok, ")");
  return tok;
}

//
// is_typename — the watershed (04_parse.md §5).
//

static bool is_typename(Token *tok) {
  // Keyword set per 04_parse.md §5.2.  35 entries; linear search.
  static const char *kw[] = {
    "void", "_Bool", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "_Complex", "__complex__",
    "struct", "union", "enum",
    "typedef", "static", "extern", "auto", "register",
    "_Thread_local", "__thread",
    "inline", "_Noreturn",
    "const", "volatile", "restrict", "_Atomic",
    "_Alignas",
    "typeof", "__typeof", "__typeof__",
    "__extension__", "__builtin_va_list", "__attribute__",
  };

  for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++)
    if (equal(tok, kw[i]))
      return true;

  // Identifier resolution: typedef name in ordinary namespace.
  if (tok->kind == TK_IDENT) {
    VarScope *vs = find_var(tok);
    return vs && vs->type_def;
  }
  return false;
}

//
// parse() — top-level driver (04a_decl.md §I).
//

Obj *parse(Token *tok) {
  // §I.1 — setup.
  globals = NULL;
  enter_scope();

  // §I.2 — top-level loop.
  while (tok->kind != TK_EOF) {
    // Stray attributes: consume.
    if (equal(tok, "__attribute__")) {
      tok = skip_attribute(tok);
      if (equal(tok, ";")) tok = tok->next;
      continue;
    }

    // Stray semicolons: consume.
    if (equal(tok, ";")) {
      tok = tok->next;
      continue;
    }

    // _Static_assert / static_assert at file scope.
    if (equal(tok, "_Static_assert") || equal(tok, "static_assert")) {
      tok = skip_static_assert(tok);
      continue;
    }

    // C89 implicit-int rule: if the leading token is an identifier
    // followed by `(`, accept it as a function with implicit `int`
    // return type.  GCC's older flag combination, used heavily in
    // the torture corpus.  Strict C99 rejects this — match canonical
    // ncc which is permissive.
    VarAttr attr = {0};
    Type *basety;
    if (!is_typename(tok)) {
      if (tok->kind == TK_IDENT && equal(tok->next, "(")) {
        basety = ty_int;
      } else {
        error_tok(tok, "parse_v2: expected declaration");
      }
    } else {
      basety = declspec(&tok, tok, &attr);
    }

    if (attr.is_typedef) {
      tok = parse_typedef(tok, basety);
      continue;
    }

    // Bare struct/union/enum declaration (no declarator follows).
    // §I.2 step 5: skip `;` and continue.  Detection: declspec
    // returned a tag type and the next token is `;`.
    if ((basety->kind == TY_STRUCT || basety->kind == TY_UNION ||
         basety->kind == TY_ENUM) && equal(tok, ";")) {
      tok = tok->next;
      continue;
    }

    // First declarator.  Subsequent declarators (after `,`) are
    // handled inside global_variable / function_declaration.
    Type *ty = declarator(&tok, tok, basety);

    if (ty->kind == TY_FUNC) {
      // §I.2 step 4: dispatch on the next token.
      if (equal(tok, "{")) {
        tok = function(tok, basety, ty, &attr);
      } else if (is_typename(tok) && !equal(tok, ";") &&
                 !equal(tok, "__attribute__") &&
                 !equal(tok, "asm") && !equal(tok, "__asm") &&
                 !equal(tok, "__asm__")) {
        // K&R-style function definition.
        tok = function(tok, basety, ty, &attr);
      } else {
        // Function declaration (no body).
        tok = function_declaration(tok, basety, ty, &attr);
      }
    } else {
      // §I.2 step 6: global variable.
      tok = global_variable(tok, basety, ty, &attr);
    }
  }

  // §I.3 — cleanup.
  leave_scope();
  return globals;
}

//
// declspec — declaration specifiers (04a_decl.md §B).
//
// Bitmask state machine: each primitive contributes a unique
// increment that, when summed, encodes the full type.  Duplicate
// `int` etc. overflow into invalid counter space and trigger the
// switch's default error arm.  LONG is intentionally additive (two
// allowed: `long long`).  Storage class, qualifiers, and attributes
// run alongside.
//
// Stubs (deep dependencies not yet implemented):
//   - struct_decl / union_decl / enum_specifier (§F)
//   - typeof family (§B.7)
//   - _Atomic ( type-name ) (§B.5 paren form; bare qualifier works)
//   - _Alignas (§B.6)
//   - attribute_list (§G)
//

static Type *struct_decl(Token **rest, Token *tok);
static Type *union_decl(Token **rest, Token *tok);
static Type *enum_specifier(Token **rest, Token *tok);
static Type *typeof_specifier(Token **rest, Token *tok);
static Type *atomic_specifier(Token **rest, Token *tok);
static Token *parse_alignas(Token *tok, VarAttr *attr);
static Token *attribute_list(Token *tok, Type *ty, VarAttr *attr);

static Type *declspec(Token **rest, Token *tok, VarAttr *attr) {
  enum {
    VOID     = 1 << 0,
    BOOL     = 1 << 2,
    CHAR     = 1 << 4,
    SHORT    = 1 << 6,
    INT      = 1 << 8,
    LONG     = 1 << 10,
    FLOAT    = 1 << 12,
    DOUBLE   = 1 << 14,
    OTHER    = 1 << 16,
    SIGNED   = 1 << 17,
    UNSIGNED = 1 << 18,
    COMPLEX  = 1 << 19,
  };

  Type *ty = ty_int;       // C default-int rule (§B.2 footnote).
  int counter = 0;
  bool is_const = false, is_volatile = false, is_atomic = false;

  while (is_typename(tok)) {
    // Storage class (§B.3) — consumed; flags only when attr non-NULL.
    if (equal(tok, "typedef") || equal(tok, "static") ||
        equal(tok, "extern") || equal(tok, "inline") ||
        equal(tok, "_Noreturn") || equal(tok, "_Thread_local") ||
        equal(tok, "__thread") || equal(tok, "register") ||
        equal(tok, "auto")) {
      if (attr) {
        if (equal(tok, "typedef"))                            attr->is_typedef = true;
        else if (equal(tok, "static"))                        attr->is_static = true;
        else if (equal(tok, "extern"))                        attr->is_extern = true;
        else if (equal(tok, "inline"))                        attr->is_inline = true;
        else if (equal(tok, "_Noreturn"))                     attr->is_noreturn = true;
        else if (equal(tok, "_Thread_local") || equal(tok, "__thread"))
          attr->is_tls = true;
        // register / auto: consumed without flag.
      }
      tok = tok->next;
      continue;
    }

    // Qualifiers (§B.4).
    if (equal(tok, "const"))    { is_const    = true; tok = tok->next; continue; }
    if (equal(tok, "volatile")) { is_volatile = true; tok = tok->next; continue; }
    if (equal(tok, "restrict") || equal(tok, "__restrict") ||
        equal(tok, "__restrict__")) {
      tok = tok->next; continue;
    }

    // _Atomic (§B.5) — bare qualifier; paren form recurses via typename_.
    if (equal(tok, "_Atomic")) {
      if (equal(tok->next, "(")) {
        ty = atomic_specifier(&tok, tok);
        is_atomic = true;
        counter += OTHER;
        continue;
      }
      is_atomic = true;
      tok = tok->next;
      continue;
    }

    // _Alignas (§B.6).
    if (equal(tok, "_Alignas")) {
      tok = parse_alignas(tok, attr);
      continue;
    }

    // __extension__ (§B.11) — silent no-op.
    if (equal(tok, "__extension__")) {
      tok = tok->next;
      continue;
    }

    // __attribute__ (§B.10).
    if (equal(tok, "__attribute__")) {
      // Per §B.10: copy_type the current ty (if not a tag type) so
      // attribute mutations don't corrupt shared singletons.  Tag
      // types share identity with the tag table and must not be
      // copied.
      if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
        ty = copy_type(ty);
      tok = attribute_list(tok->next, ty, attr);
      continue;
    }

    // typeof family (§B.7).
    if (equal(tok, "typeof") || equal(tok, "__typeof") ||
        equal(tok, "__typeof__")) {
      ty = typeof_specifier(&tok, tok);
      counter += OTHER;
      continue;
    }

    // Tag specifiers (§B.9 first half).
    if (equal(tok, "struct")) { ty = struct_decl(&tok, tok->next); counter += OTHER; continue; }
    if (equal(tok, "union"))  { ty = union_decl(&tok, tok->next);  counter += OTHER; continue; }
    if (equal(tok, "enum"))   { ty = enum_specifier(&tok, tok->next); counter += OTHER; continue; }

    // __builtin_va_list (§B.8) — pointer to void at the C type level.
    if (equal(tok, "__builtin_va_list")) {
      ty = pointer_to(ty_void);
      counter += OTHER;
      tok = tok->next;
      continue;
    }

    // Typedef-name resolution (§B.9 second half).  Guarded by
    // counter == 0 so `int x` doesn't try to interpret `x` as a
    // typedef.  Once a primitive specifier is in, an identifier
    // here is the declarator name and we must terminate.
    if (tok->kind == TK_IDENT) {
      if (counter != 0) break;
      VarScope *vs = find_var(tok);
      if (!vs || !vs->type_def) break;   // is_typename was true → must be typedef
      ty = vs->type_def;
      counter += OTHER;
      tok = tok->next;
      continue;
    }

    // Primitives — accumulate into counter, then evaluate.
    if      (equal(tok, "void"))                                counter += VOID;
    else if (equal(tok, "_Bool"))                               counter += BOOL;
    else if (equal(tok, "char"))                                counter += CHAR;
    else if (equal(tok, "short"))                               counter += SHORT;
    else if (equal(tok, "int"))                                 counter += INT;
    else if (equal(tok, "long"))                                counter += LONG;
    else if (equal(tok, "float"))                               counter += FLOAT;
    else if (equal(tok, "double"))                              counter += DOUBLE;
    else if (equal(tok, "signed"))                              counter += SIGNED;
    else if (equal(tok, "unsigned"))                            counter += UNSIGNED;
    else if (equal(tok, "_Complex") || equal(tok, "__complex__")) counter += COMPLEX;
    else error_tok(tok, "parse_v2: declspec: unhandled type specifier");

    // Canonical-type switch (§B.2).  Order does not matter
    // normatively; arms grouped for readability.
    switch (counter) {
    case VOID:                                                          ty = ty_void;       break;
    case BOOL:                                                          ty = ty_bool;       break;

    case CHAR:
    case SIGNED + CHAR:                                                 ty = ty_char;       break;
    case UNSIGNED + CHAR:                                               ty = ty_uchar;      break;

    case SHORT:
    case SHORT + INT:
    case SIGNED + SHORT:
    case SIGNED + SHORT + INT:                                          ty = ty_short;      break;
    case UNSIGNED + SHORT:
    case UNSIGNED + SHORT + INT:                                        ty = ty_ushort;     break;

    case INT:
    case SIGNED:
    case SIGNED + INT:                                                  ty = ty_int;        break;
    case UNSIGNED:
    case UNSIGNED + INT:                                                ty = ty_uint;       break;

    case LONG:
    case LONG + INT:
    case SIGNED + LONG:
    case SIGNED + LONG + INT:                                           ty = ty_long;       break;
    case UNSIGNED + LONG:
    case UNSIGNED + LONG + INT:                                         ty = ty_ulong;      break;

    case LONG + LONG:
    case LONG + LONG + INT:
    case SIGNED + LONG + LONG:
    case SIGNED + LONG + LONG + INT:                                    ty = ty_longlong;   break;
    case UNSIGNED + LONG + LONG:
    case UNSIGNED + LONG + LONG + INT:                                  ty = ty_ulonglong;  break;

    case FLOAT:                                                         ty = ty_float;      break;
    case DOUBLE:                                                        ty = ty_double;     break;
    case LONG + DOUBLE:                                                 ty = ty_ldouble;    break;

    case COMPLEX:
    case COMPLEX + DOUBLE:                                              ty = complex_type(ty_double);     break;
    case COMPLEX + FLOAT:                                               ty = complex_type(ty_float);      break;
    case COMPLEX + LONG + DOUBLE:                                       ty = complex_type(ty_ldouble);    break;

    case COMPLEX + INT:
    case COMPLEX + SIGNED:
    case COMPLEX + SIGNED + INT:                                        ty = complex_type(ty_int);        break;
    case COMPLEX + UNSIGNED:
    case COMPLEX + UNSIGNED + INT:                                      ty = complex_type(ty_uint);       break;

    case COMPLEX + LONG:
    case COMPLEX + LONG + INT:
    case COMPLEX + SIGNED + LONG:
    case COMPLEX + SIGNED + LONG + INT:                                 ty = complex_type(ty_long);       break;
    case COMPLEX + UNSIGNED + LONG:
    case COMPLEX + UNSIGNED + LONG + INT:                               ty = complex_type(ty_ulong);      break;

    case COMPLEX + LONG + LONG:
    case COMPLEX + LONG + LONG + INT:
    case COMPLEX + SIGNED + LONG + LONG:
    case COMPLEX + SIGNED + LONG + LONG + INT:                          ty = complex_type(ty_longlong);   break;
    case COMPLEX + UNSIGNED + LONG + LONG:
    case COMPLEX + UNSIGNED + LONG + LONG + INT:                        ty = complex_type(ty_ulonglong);  break;

    case COMPLEX + SHORT:
    case COMPLEX + SHORT + INT:
    case COMPLEX + SIGNED + SHORT:
    case COMPLEX + SIGNED + SHORT + INT:                                ty = complex_type(ty_short);      break;
    case COMPLEX + UNSIGNED + SHORT:
    case COMPLEX + UNSIGNED + SHORT + INT:                              ty = complex_type(ty_ushort);     break;

    case COMPLEX + CHAR:
    case COMPLEX + SIGNED + CHAR:                                       ty = complex_type(ty_char);       break;
    case COMPLEX + UNSIGNED + CHAR:                                     ty = complex_type(ty_uchar);      break;

    default:
      error_tok(tok, "parse_v2: declspec: invalid type");
    }

    tok = tok->next;
  }

  // §B.12 post-loop processing.
  if (is_const || is_volatile || is_atomic) {
    ty = copy_type(ty);
    ty->is_const    = is_const;
    ty->is_volatile = is_volatile;
    ty->is_atomic   = is_atomic;
  }

  // mode_kind and vector_size are populated by attribute_list, which
  // is currently stubbed; the bodies below are unreachable until §G
  // lands but match the spec contract.
  if (attr && attr->mode_kind) {
    bool unsigned_ = ty->is_unsigned || (counter & UNSIGNED);
    Type *new_ty = NULL;
    switch (attr->mode_kind) {
    case 1: new_ty = unsigned_ ? ty_uchar  : ty_char;  break;
    case 2: new_ty = unsigned_ ? ty_ushort : ty_short; break;
    case 4: new_ty = unsigned_ ? ty_uint   : ty_int;   break;
    case 8: new_ty = unsigned_ ? ty_ulong  : ty_long;  break;
    }
    if (new_ty) ty = copy_type(new_ty);
  }

  if (attr && attr->vector_size && !is_vector(ty) && ty->size > 0)
    ty = vector_of(copy_type(ty), attr->vector_size);

  *rest = tok;
  return ty;
}

//
// declspec deep-dependency stubs.
//

// struct_members — `{` already consumed.  Comma-separated decls
// terminated by `}`.  Anonymous-member support: a declspec with
// no following declarator and a STRUCT/UNION type creates a member
// with name = NULL whose `ty` is the inner aggregate (find_member
// recurses through these — see §F.8).  Bitfield, flexible-array,
// per-member attribute honoring deferred.
static Member *struct_members(Token **rest, Token *tok) {
  Member head = {0};
  Member *cur = &head;
  int idx = 0;
  while (!equal(tok, "}")) {
    VarAttr attr = {0};
    Type *basety = declspec(&tok, tok, &attr);

    // Anonymous member: the declspec was a struct/union and the
    // declarator is empty (`;` immediately follows).
    if (equal(tok, ";") &&
        (basety->kind == TY_STRUCT || basety->kind == TY_UNION)) {
      Member *m = calloc_checked(1, sizeof(Member));
      m->ty = basety;
      m->name = NULL;
      m->tok = NULL;
      m->idx = idx++;
      m->align = basety->align;
      cur = cur->next = m;
      tok = tok->next;
      continue;
    }

    bool first = true;
    while (!equal(tok, ";")) {
      if (!first) tok = skip(tok, ",");
      first = false;
      Type *ty = declarator(&tok, tok, basety);

      // Anonymous bitfield (zero-width or named placeholder):
      // `int : 0;` aligns the next bitfield; `int : 4;` reserves
      // bits without a name.  Skip name requirement when bitfield.
      bool is_bitfield = false;
      int bit_width = 0;
      if (equal(tok, ":")) {
        is_bitfield = true;
        bit_width = (int)const_expr_val(&tok, tok->next);
      } else if (!ty->name) {
        error_tok(tok, "struct member requires a name");
      }
      if (equal(tok, "__attribute__"))
        tok = attribute_list(tok->next, ty, &attr);

      Member *m = calloc_checked(1, sizeof(Member));
      m->ty = ty;
      m->name = ty->name;
      m->tok = ty->name;
      m->idx = idx++;
      m->align = attr.align ? attr.align : ty->align;
      m->is_bitfield = is_bitfield;
      m->bit_width = bit_width;
      cur = cur->next = m;
    }
    tok = skip(tok, ";");
  }
  // Flexible array member (last member with incomplete array type):
  // normalize array_len to 0 so struct_layout treats it as zero-size
  // and the surrounding struct's size accounts only for the declared
  // members.  Mirrors canonical parse.c §1.6 (line 1325-1329).
  if (cur != &head && cur->ty->kind == TY_ARRAY && cur->ty->array_len < 0) {
    cur->ty = array_of(cur->ty->base, 0);
  }
  *rest = skip(tok, "}");
  return head.next;
}

// struct_layout — assign offsets and compute size + align.
// Bitfield handling per 04a §F.5: bitfields pack into storage units
// of their declared type's size, with a running bit counter that
// resyncs from `offset * 8` when transitioning from non-bitfield to
// bitfield.  Zero-width bitfields force the running counter to the
// next storage-unit boundary without allocating a slot.
static void struct_layout(Type *ty) {
  long offset = 0;
  long bits = 0;
  bool in_bf = false;
  int max_align = 1;
  for (Member *m = ty->members; m; m = m->next) {
    int al = m->align ? m->align : m->ty->align;
    if (al < 1) al = 1;
    if (m->is_bitfield) {
      int unit = (int)m->ty->size;
      if (!in_bf) {
        bits = offset * 8;
        in_bf = true;
      }
      if (m->bit_width == 0) {
        // Align bits up to next storage-unit boundary.
        long unit_bits = (long)unit * 8;
        bits = (bits + unit_bits - 1) / unit_bits * unit_bits;
        m->offset = bits / 8;
        m->bit_offset = 0;
        // No slot allocated.
      } else {
        long unit_bits = (long)unit * 8;
        long start = bits;
        long start_unit = start / unit_bits;
        long end_unit = (start + m->bit_width - 1) / unit_bits;
        if (start_unit != end_unit) {
          // Doesn't fit in current unit — advance to next.
          bits = end_unit * unit_bits;
        }
        m->offset = (bits / unit_bits) * unit;
        m->bit_offset = (int)(bits - (m->offset * 8));
        bits += m->bit_width;
      }
      offset = (bits + 7) / 8;
      if (al > max_align) max_align = al;
      continue;
    }
    // Non-bitfield: re-sync.
    in_bf = false;
    bits = 0;
    if (!ty->is_packed)
      offset = align_to(offset, al);
    m->offset = offset;
    offset += m->ty->size;
    if (al > max_align) max_align = al;
  }
  if (ty->is_packed) {
    if (ty->align < 1) ty->align = 1;
    ty->size = offset;
  } else {
    if (max_align > ty->align) ty->align = max_align;
    ty->size = align_to(offset, ty->align);
  }
}

// union_layout — all members at offset 0, size = max member size,
// align = max member align.
static void union_layout(Type *ty) {
  long size = 0;
  int max_align = 1;
  for (Member *m = ty->members; m; m = m->next) {
    int al = m->align ? m->align : m->ty->align;
    if (al < 1) al = 1;
    m->offset = 0;
    if (m->ty->size > size) size = m->ty->size;
    if (al > max_align) max_align = al;
  }
  ty->align = max_align;
  ty->size = align_to(size, max_align);
}

// struct_decl / union_decl share shape (04a §F.2).  Subset:
// optional tag, optional body.  Forward refs return / create
// incomplete types in the tag namespace.  Pre-tag and post-tag
// __attribute__ blobs are consumed-and-ignored for now (real
// honoring lands with attribute_list).
static Type *struct_or_union(Token **rest, Token *tok, bool is_union) {
  // §F.2 step 1: pre-tag __attribute__.  We don't have a temporary
  // Type yet, so apply to the final type after creation by routing
  // through a stack-local VarAttr that we re-apply when ty exists.
  VarAttr early_attr = {0};
  Type early_ty = {0};
  while (equal(tok, "__attribute__"))
    tok = attribute_list(tok->next, &early_ty, &early_attr);

  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  // §F.2 step 3: between-tag-and-body __attribute__.
  while (equal(tok, "__attribute__"))
    tok = attribute_list(tok->next, &early_ty, &early_attr);

  // Forward reference / lookup.
  if (tag && !equal(tok, "{")) {
    TagScope *ts = find_tag(tag);
    if (ts) {
      *rest = tok;
      return ts->ty;
    }
    Type *ty = struct_type();
    ty->kind = is_union ? TY_UNION : TY_STRUCT;
    ty->size = -1;  // incomplete
    push_tag_scope(tag, ty);
    *rest = tok;
    return ty;
  }

  tok = skip(tok, "{");

  // Reuse incomplete tag if present.
  Type *ty = NULL;
  if (tag) {
    TagScope *ts = find_tag(tag);
    if (ts && ts->ty->size < 0)
      ty = ts->ty;
  }
  if (!ty) {
    ty = struct_type();
    ty->kind = is_union ? TY_UNION : TY_STRUCT;
  }
  // Apply early attributes before laying out so packed/aligned
  // affect the layout (§F.2 step 5).
  if (early_ty.is_packed) ty->is_packed = true;
  if (early_attr.align && early_attr.align > ty->align) ty->align = early_attr.align;

  // Register the tag BEFORE parsing members, so self-referential
  // pointer fields (`struct S { struct S *next; }`) resolve to this
  // type rather than creating a fresh incomplete one.
  if (tag) {
    TagScope *ts = find_tag(tag);
    if (!ts)
      push_tag_scope(tag, ty);
  }

  ty->members = struct_members(&tok, tok);
  if (is_union)
    union_layout(ty);
  else
    struct_layout(ty);

  // §F.2 step 5 last bullet: trailing __attribute__ may force packed
  // re-layout.
  while (equal(tok, "__attribute__")) {
    bool was_packed = ty->is_packed;
    tok = attribute_list(tok->next, ty, NULL);
    if (!was_packed && ty->is_packed && !is_union)
      struct_layout(ty);
  }

  *rest = tok;
  return ty;
}

static Type *struct_decl(Token **rest, Token *tok) {
  return struct_or_union(rest, tok, false);
}

static Type *union_decl(Token **rest, Token *tok) {
  return struct_or_union(rest, tok, true);
}

// find_member — direct linear search.  Returns the matching member
// in the immediate type; anonymous nested members are handled at
// the call site by struct_ref via recursion.
static Member *find_member(Type *ty, Token *name) {
  for (Member *m = ty->members; m; m = m->next) {
    if (m->name && name->len == m->name->len &&
        !strncmp(m->name->loc, name->loc, name->len))
      return m;
  }
  return NULL;
}

// has_member_named — does the (possibly nested anonymous) struct/
// union type ty contain a member with the given name?  Walks
// anonymous members recursively.
static bool has_member_named(Type *ty, Token *name) {
  for (Member *m = ty->members; m; m = m->next) {
    if (m->name) {
      if (name->len == m->name->len &&
          !strncmp(m->name->loc, name->loc, name->len))
        return true;
    } else if (m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION) {
      if (has_member_named(m->ty, name))
        return true;
    }
  }
  return false;
}

// struct_ref — wrap node in ND_MEMBER for `node . name`.  Caller
// must have applied ND_DEREF for `->`.  When `name` is not directly
// a member, recurse through anonymous members building an
// ND_MEMBER(ND_MEMBER(node, anon), real) chain so codegen sees the
// correct cumulative offset.
static Node *struct_ref(Node *node, Token *name) {
  add_type(node);
  if (node->ty->kind != TY_STRUCT && node->ty->kind != TY_UNION)
    error_tok(node->tok, "not a struct or union");

  // Follow origin chain to the completed type with members — covers
  // forward decls of `const struct B *` where the const-qualified copy
  // was made before B's body was parsed.  Mirrors canonical struct_ref.
  Type *cty = node->ty;
  while (cty->origin && !cty->members &&
         (cty->kind == TY_STRUCT || cty->kind == TY_UNION))
    cty = cty->origin;

  // Direct hit.
  Member *m = find_member(cty, name);
  if (m) {
    Node *n = new_unary(ND_MEMBER, node, name);
    n->member = m;
    return n;
  }

  // Walk anonymous members: each anon hop adds an ND_MEMBER node.
  for (Member *am = cty->members; am; am = am->next) {
    if (am->name) continue;
    if (am->ty->kind != TY_STRUCT && am->ty->kind != TY_UNION) continue;
    if (!has_member_named(am->ty, name)) continue;
    Node *anon_ref = new_unary(ND_MEMBER, node, name);
    anon_ref->member = am;
    return struct_ref(anon_ref, name);
  }

  error_tok(name, "no such member");
}

// enum_specifier — `enum` already consumed.  04a §F.7 subset:
//   - enum Tag                 → forward reference / lookup
//   - enum [Tag] { name [=v], ... }
// C23 fixed underlying-type (`enum Tag : T { ... }`) is parsed and
// discarded.
static Type *enum_specifier(Token **rest, Token *tok) {
  // Optional tag.
  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  // Forward reference: `enum Tag` with no body — look up or create
  // an incomplete entry.
  if (tag && !equal(tok, "{")) {
    TagScope *ts = find_tag(tag);
    if (ts) {
      if (ts->ty->kind != TY_ENUM)
        error_tok(tag, "not an enum tag");
      *rest = tok;
      return ts->ty;
    }
    Type *ty = enum_type();
    push_tag_scope(tag, ty);
    *rest = tok;
    return ty;
  }

  // Body required.
  // C23 fixed underlying type — `: type-name` before `{`.
  if (equal(tok, ":")) {
    typename_(&tok, tok->next);
    // discard
  }
  tok = skip(tok, "{");

  Type *ty = enum_type();
  int val = 0;
  bool first = true;
  while (!equal(tok, "}")) {
    if (!first) tok = skip(tok, ",");
    first = false;
    if (equal(tok, "}")) break; // trailing comma
    if (tok->kind != TK_IDENT)
      error_tok(tok, "expected identifier");
    char *name = strndup_checked(tok->loc, tok->len);
    Token *name_tok = tok;
    tok = tok->next;
    if (equal(tok, "=")) {
      val = (int)const_expr_val(&tok, tok->next);
    }
    // Optional trailing __attribute__((...)) per spec note.
    if (equal(tok, "__attribute__"))
      tok = attribute_list(tok->next, NULL, NULL);

    VarScope *vs = push_scope(name);
    vs->enum_ty  = ty;
    vs->enum_val = val++;
    (void)name_tok;
  }
  *rest = skip(tok, "}");

  if (tag)
    push_tag_scope(tag, ty);
  return ty;
}

// typeof_specifier — `typeof` / `__typeof` / `__typeof__` keyword
// is `tok`.  04a §B.7.  Form: `typeof ( type-name | expr )`.
static Type *typeof_specifier(Token **rest, Token *tok) {
  tok = tok->next;  // consume the keyword
  tok = skip(tok, "(");
  Type *ty;
  if (is_typename(tok)) {
    ty = typename_(&tok, tok);
  } else {
    Node *e = expr(&tok, tok);
    add_type(e);
    ty = e->ty ? e->ty : ty_int;
  }
  *rest = skip(tok, ")");
  return ty;
}

static Type *atomic_specifier(Token **rest, Token *tok) {
  (void)rest;
  error_tok(tok, "parse_v2: _Atomic(type-name) not yet implemented");
}

// _Alignas — `_Alignas` keyword is `tok`.  04a §B.6.
//   _Alignas ( const-expr )    → attr->align
//   _Alignas ( type-name )     → attr->align = ty->align
static Token *parse_alignas(Token *tok, VarAttr *attr) {
  if (!attr)
    error_tok(tok, "_Alignas is not allowed here");
  tok = tok->next;  // consume keyword
  tok = skip(tok, "(");
  int al;
  if (is_typename(tok)) {
    Type *ty = typename_(&tok, tok);
    al = ty->align;
  } else {
    al = (int)const_expr_val(&tok, tok);
  }
  tok = skip(tok, ")");
  if (al > attr->align) attr->align = al;
  return tok;
}

// attribute_list — 04a §G subset.  Caller has consumed the
// `__attribute__` token; `tok` points at the first `(` of `((`.
// Loops over chained `__attribute__((...))` clauses.  Honors
// packed / aligned / vector_size / mode / alias.  Other attributes
// are parsed-and-ignored.  Per-attribute argument lists are
// brace-balanced-consumed when not interpreted.
static bool tok_name_eq(Token *tok, const char *name) {
  size_t n = strlen(name);
  return tok->len == (int)n && !strncmp(tok->loc, name, n);
}

static Token *attribute_list(Token *tok, Type *ty, VarAttr *attr) {
  for (;;) {
    tok = skip(tok, "(");
    tok = skip(tok, "(");
    bool first = true;
    while (!equal(tok, ")")) {
      if (!first) tok = skip(tok, ",");
      first = false;
      if (equal(tok, ")")) break;

      Token *name_tok = tok;
      tok = tok->next;
      bool has_args = equal(tok, "(");

      if (tok_name_eq(name_tok, "packed") || tok_name_eq(name_tok, "__packed__")) {
        if (ty && ty->kind != TY_FUNC) ty->is_packed = true;
        if (has_args) {
          // packed takes no args; tolerate empty `()`.
          tok = tok->next;
          tok = skip(tok, ")");
        }
      } else if (tok_name_eq(name_tok, "aligned") || tok_name_eq(name_tok, "__aligned__")) {
        int n = 16;
        if (has_args) {
          tok = tok->next;
          n = (int)const_expr_val(&tok, tok);
          tok = skip(tok, ")");
        }
        if (attr) attr->align = n;
        if (ty) ty->align = n;
      } else if (tok_name_eq(name_tok, "vector_size") ||
                 tok_name_eq(name_tok, "__vector_size__")) {
        int n = 0;
        if (has_args) {
          tok = tok->next;
          n = (int)const_expr_val(&tok, tok);
          tok = skip(tok, ")");
        }
        if (attr) attr->vector_size = n;
      } else if (tok_name_eq(name_tok, "mode") || tok_name_eq(name_tok, "__mode__")) {
        // mode(QI|HI|SI|DI|word) — map to byte width 1/2/4/8.
        int kind = 0;
        if (has_args) {
          tok = tok->next;
          if (tok->kind == TK_IDENT) {
            if (tok_name_eq(tok, "QI") || tok_name_eq(tok, "__QI__"))      kind = 1;
            else if (tok_name_eq(tok, "HI") || tok_name_eq(tok, "__HI__")) kind = 2;
            else if (tok_name_eq(tok, "SI") || tok_name_eq(tok, "__SI__")) kind = 4;
            else if (tok_name_eq(tok, "DI") || tok_name_eq(tok, "__DI__")) kind = 8;
            else if (tok_name_eq(tok, "word"))                              kind = 8;
            tok = tok->next;
          }
          tok = skip(tok, ")");
        }
        if (attr) attr->mode_kind = kind;
      } else if (tok_name_eq(name_tok, "alias") || tok_name_eq(name_tok, "__alias__")) {
        if (has_args) {
          // brace-balanced skip; honoring would set attr->alias_target,
          // but the field doesn't live on VarAttr currently.  Recorded
          // as parsed-and-ignored for now.
          tok = tok->next;
          int depth = 1;
          while (depth > 0 && tok->kind != TK_EOF) {
            if (equal(tok, "(")) depth++;
            else if (equal(tok, ")")) { depth--; if (depth == 0) break; }
            tok = tok->next;
          }
          tok = skip(tok, ")");
        }
      } else {
        // Parsed-and-ignored: consume optional ( ... ) brace-balanced.
        if (has_args) {
          tok = tok->next;
          int depth = 1;
          while (depth > 0 && tok->kind != TK_EOF) {
            if (equal(tok, "(")) depth++;
            else if (equal(tok, ")")) { depth--; if (depth == 0) break; }
            tok = tok->next;
          }
          tok = skip(tok, ")");
        }
      }
    }
    tok = skip(tok, ")");
    tok = skip(tok, ")");

    if (!equal(tok, "__attribute__")) break;
    tok = tok->next;
  }
  return tok;
}

// parse_typedef — 04c §B.5 / 04a §I.2.  Comma-separated declarators
// after a `typedef` declspec; each is bound as a typedef-name in
// the current scope.  VLA-typedef compute_vla_size emission is
// deferred (depends on VLA support).
static Token *parse_typedef(Token *tok, Type *basety) {
  bool first = true;
  while (!equal(tok, ";")) {
    if (!first) tok = skip(tok, ",");
    first = false;

    Type *ty = declarator(&tok, tok, basety);
    if (equal(tok, "__attribute__"))
      tok = attribute_list(tok->next, ty, NULL);
    if (!ty->name)
      error_tok(tok, "typedef declarator requires a name");

    char *name = strndup_checked(ty->name->loc, ty->name->len);
    push_scope(name)->type_def = ty;
  }
  return skip(tok, ";");
}

// function — definition body parsing (04c_stmt.md §F).  basety is
// retained from the caller for future K&R param-merging; currently
// unused since we don't yet emit K&R-style parameter promotions.
static Token *function(Token *tok, Type *basety, Type *ty, VarAttr *attr) {
  (void)basety;

  if (!ty->name)
    error_tok(tok, "parse_v2: function definition requires a name");

  char *name = strndup_checked(ty->name->loc, ty->name->len);

  // §F.1 — create / resolve Obj.  Forward decls: re-use the existing
  // global by name lookup so codegen sees a single Obj.  Simplified
  // for now: always new_gvar, which will shadow any prior decl in
  // the symbol chain.  (Future: dedupe via push_scope's var lookup.)
  Obj *fn = new_gvar(name, ty);
  fn->is_function = true;
  fn->is_definition = true;
  fn->is_static = attr->is_static;
  fn->is_inline = attr->is_inline;
  fn->is_variadic = ty->is_variadic;

  // Save per-function state (§F.1 step 3).
  Obj  *saved_current_fn    = current_fn;
  Obj  *saved_locals        = locals;
  Node *saved_gotos         = gotos;
  Node *saved_labels        = labels;
  char *saved_brk           = brk_label;
  char *saved_cont          = cont_label;
  Node *saved_switch        = current_switch;
  current_fn = fn;
  locals = NULL;
  gotos = NULL;
  labels = NULL;
  brk_label = NULL;
  cont_label = NULL;
  current_switch = NULL;

  enter_scope();

  // §F.2 — register parameters.  new_lvar prepends to `locals`, so
  // walking ty->params front-to-back produces locals in
  // reverse-declaration order; reverse it once before exposing as
  // fn->params (which must be declaration order for codegen to
  // assign register slots correctly).
  for (Type *pt = ty->params; pt; pt = pt->next) {
    char *pname;
    if (pt->name && pt->name->len > 0)
      pname = strndup_checked(pt->name->loc, pt->name->len);
    else
      pname = new_unique_name();
    new_lvar(pname, pt);
  }
  Obj *rev = NULL;
  while (locals) {
    Obj *n = locals->next;
    locals->next = rev;
    rev = locals;
    locals = n;
  }
  locals = rev;
  fn->params = locals;

  // §F.4 — alloca infrastructure.  Always emit __alloca_bottom__.
  // Variadic gets __va_area__.
  if (fn->is_variadic) {
    Obj *va = new_lvar("__va_area__", pointer_to(ty_char));
    va->align = 8;
    fn->va_area = va;
  }
  fn->alloca_bottom = new_lvar("__alloca_bottom__", pointer_to(ty_char));

  // §F.3 — K&R-style parameter declarations (between `)` and `{`).
  // For each declaration, look up the parameter name in fn->params
  // and update its type / align.  Excess K&R declarators (no
  // matching parameter) are an error per the spec.
  while (!equal(tok, "{")) {
    VarAttr kr_attr = {0};
    Type *kr_basety = declspec(&tok, tok, &kr_attr);
    bool first = true;
    while (!equal(tok, ";")) {
      if (!first) tok = skip(tok, ",");
      first = false;
      Type *kr_ty = declarator(&tok, tok, kr_basety);
      if (!kr_ty->name)
        error_tok(tok, "K&R declarator requires a name");
      // Look up matching param.
      bool found = false;
      for (Obj *p = fn->params; p; p = p->next) {
        if (!strcmp(p->name, strndup_checked(kr_ty->name->loc, kr_ty->name->len))) {
          p->ty = kr_ty;
          if (kr_attr.align) p->align = kr_attr.align;
          found = true;
          break;
        }
      }
      if (!found)
        error_tok(kr_ty->name, "parameter name not in K&R parameter list");
    }
    tok = skip(tok, ";");
  }

  // §F.5 — body.
  tok = skip(tok, "{");
  Node *body = compound_stmt(&tok, tok);
  add_type(body);

  // Resolve labels (04c §E.5).
  // Direct goto:  copy l->unique_label into g->unique_label.
  // ND_LABEL_VAL: the label was minted at the &&-site, so copy
  //               g->unique_label into the matching ND_LABEL so
  //               codegen emits a single label for both.
  for (Node *g = gotos; g; g = g->goto_next) {
    if (g->kind == ND_LABEL_VAL) {
      for (Node *l = labels; l; l = l->goto_next) {
        if (g->label && l->label && !strcmp(g->label, l->label)) {
          l->unique_label = g->unique_label;
          break;
        }
      }
      continue;
    }
    if (g->unique_label) continue;  // break/continue resolved already
    for (Node *l = labels; l; l = l->goto_next) {
      if (g->label && l->label && !strcmp(g->label, l->label)) {
        g->unique_label = l->unique_label;
        break;
      }
    }
    if (!g->unique_label)
      error_tok(g->tok, "use of undeclared label");
  }

  fn->body = body;
  fn->locals = locals;

  leave_scope();

  // Restore per-function state.
  current_fn     = saved_current_fn;
  locals         = saved_locals;
  gotos          = saved_gotos;
  labels         = saved_labels;
  brk_label      = saved_brk;
  cont_label     = saved_cont;
  current_switch = saved_switch;

  return tok;
}

//
// Declarator and friends (04a_decl.md §C, §D, §E).
//

// pointers — process each `*` and the qualifier sequence after it
// (§C.1).  Multiple `*`s nest left-to-right via pointer_to().
static Type *pointers(Token **rest, Token *tok, Type *ty) {
  while (equal(tok, "*")) {
    ty = pointer_to(ty);
    tok = tok->next;
    while (equal(tok, "const") || equal(tok, "volatile") ||
           equal(tok, "restrict") || equal(tok, "__restrict") ||
           equal(tok, "__restrict__") || equal(tok, "_Atomic")) {
      if (equal(tok, "const"))    ty->is_const    = true;
      if (equal(tok, "volatile")) ty->is_volatile = true;
      // restrict / __restrict / __restrict__ / _Atomic: consumed
      // without setting flags (ncc does not enforce restrict
      // semantics; bare _Atomic on a pointer is a no-op).
      tok = tok->next;
    }
    // Optional __attribute__ that attaches to this pointer.
    if (equal(tok, "__attribute__"))
      tok = attribute_list(tok->next, ty, NULL);
  }
  *rest = tok;
  return ty;
}

// Look-ahead to disambiguate a `(` that could open either a nested
// declarator or a function parameter list.  This is a simplified
// heuristic vs the spec's try-parse approach (§C.3): if `(` is
// followed by `)` or a typename, it's a parameter list; otherwise
// (`*`, `(`, identifier-not-typename) it's a nested declarator.
//
// Known gap: K&R-style declarations like `int f(a, b);` (identifier
// list, no types) would be misclassified here.  We don't see these
// in the self-host corpus.
static bool looks_like_nested_declarator(Token *tok) {
  // `tok` is the `(`.  Peek one ahead.
  Token *next = tok->next;
  if (equal(next, ")")) return false;       // `()` — empty params.
  if (is_typename(next)) return false;      // typename — params.
  return true;                              // `*`, `(`, ident — nested.
}

// declarator — pointers + (nested declarator | identifier) + suffix.
static Type *declarator(Token **rest, Token *tok, Type *ty) {
  // §C.1 — pointers.
  ty = pointers(&tok, tok, ty);

  // §C.2 — post-pointer __attribute__.
  if (equal(tok, "__attribute__"))
    tok = attribute_list(tok->next, ty, NULL);

  // §C.3 — nested declarator.
  if (equal(tok, "(") && looks_like_nested_declarator(tok)) {
    Token *inner_start = tok->next;
    Type placeholder = {0};
    // Walk the inner declarator into a placeholder, find the matching `)`,
    // then build the suffix and patch the placeholder.
    Token *after_inner;
    declarator(&after_inner, inner_start, &placeholder);
    Token *close = skip(after_inner, ")");
    Type *real = type_suffix(&tok, close, ty);
    *rest = tok;
    // Re-run the inner declarator against the actual outer type.
    Type *result = declarator(&after_inner, inner_start, real);
    return result;
  }

  // §C.4 — identifier (terminal).  Optional for abstract declarators.
  Token *name = NULL;
  if (tok->kind == TK_IDENT) {
    name = tok;
    tok = tok->next;
  }

  // §C.5 — type suffix.
  ty = type_suffix(&tok, tok, ty);

  // §C.6 — result construction.  Tag types share identity with the
  // tag table (don't copy); everything else gets a fresh copy so
  // setting `name` doesn't mutate a shared singleton.
  if (ty->kind != TY_STRUCT && ty->kind != TY_UNION && ty->kind != TY_ENUM)
    ty = copy_type(ty);
  ty->name = name;
  ty->name_pos = name;

  *rest = tok;
  return ty;
}

// type_suffix — `(...)` (function), `[...]` (array), or ε.
static Type *type_suffix(Token **rest, Token *tok, Type *ty) {
  if (equal(tok, "("))
    return func_params(rest, tok->next, ty);
  if (equal(tok, "["))
    return array_dimensions(rest, tok->next, ty);
  *rest = tok;
  return ty;
}

// func_params — `(` already consumed; parses the parameter list and
// the closing `)`.  Returns a TY_FUNC.
static Type *func_params(Token **rest, Token *tok, Type *ty) {
  // §E.1 — empty `()`.  Treated as variadic for K&R compatibility.
  if (equal(tok, ")")) {
    *rest = tok->next;
    Type *fn = func_type(ty);
    fn->is_variadic = true;
    return fn;
  }

  // §E.2 — `(void)`.
  if (equal(tok, "void") && equal(tok->next, ")")) {
    *rest = tok->next->next;
    return func_type(ty);
  }

  // §E.3 — normal parameter list.  Push a scope so VLA dimensions in
  // later parameters can reference earlier names.
  enter_scope();
  Type head = {0};
  Type *cur = &head;
  bool is_variadic = false;
  bool first = true;

  while (!equal(tok, ")")) {
    if (!first)
      tok = skip(tok, ",");
    first = false;

    // Variadic terminator.
    if (equal(tok, "...")) {
      is_variadic = true;
      tok = tok->next;
      break;
    }

    Type *basety = declspec(&tok, tok, NULL);
    Type *param_ty = declarator(&tok, tok, basety);

    // Trailing __attribute__((...)) on this parameter.
    if (equal(tok, "__attribute__"))
      tok = attribute_list(tok->next, param_ty, NULL);

    // Bind name into parameter scope so subsequent parameters'
    // VLA dimensions can reference it.
    if (param_ty->name) {
      char *pname = strndup_checked(param_ty->name->loc, param_ty->name->len);
      // Lightweight bind: a VarScope entry with no Obj — find_var
      // will return it for typename probes; full Obj attach happens
      // in `function` when the body scope is built.
      push_scope(pname);
    }

    // §E.4 — parameter type adjustments.
    if (param_ty->kind == TY_ARRAY || param_ty->kind == TY_VLA) {
      Token *vla_tok = (param_ty->kind == TY_VLA) ? param_ty->vla_dim_tok : NULL;
      Token *name_tok = param_ty->name;
      param_ty = pointer_to(param_ty->base);
      param_ty->name = name_tok;
      param_ty->vla_dim_tok = vla_tok;
    } else if (param_ty->kind == TY_FUNC) {
      Token *name_tok = param_ty->name;
      param_ty = pointer_to(param_ty);
      param_ty->name = name_tok;
    }

    cur = cur->next = copy_type(param_ty);
  }

  // §E.3 — empty observed loop body (only `...` or weird empty)
  // makes the function variadic.
  if (cur == &head)
    is_variadic = true;

  leave_scope();

  Type *fn = func_type(ty);
  fn->params = head.next;
  fn->is_variadic = is_variadic;
  *rest = skip(tok, ")");
  return fn;
}

// array_dimensions — `[` already consumed.  Subset:
//   [ ]              → array_of(rest_ty, -1)
//   [ const-expr ]   → array_of(rest_ty, val)
//   [ runtime-expr ] → vla_of (recorded; runtime emission is
//                      compute_vla_size's job, deferred).
// Leading [static] and qualifiers are consumed for compatibility
// (ncc does not enforce their semantics).
static Type *array_dimensions(Token **rest, Token *tok, Type *ty) {
  // §D.1 — leading qualifiers and `static`.
  while (equal(tok, "static") || equal(tok, "const") ||
         equal(tok, "volatile") || equal(tok, "restrict") ||
         equal(tok, "__restrict") || equal(tok, "__restrict__"))
    tok = tok->next;

  // §D.2 — empty `[]`.
  if (equal(tok, "]")) {
    Type *rest_ty = type_suffix(rest, tok->next, ty);
    return array_of(rest_ty, -1);
  }

  // §D.3 / §D.4 — dimension expression.  Constant test reuses
  // try_eval_node directly (more permissive than the spec's
  // structural test, but functionally equivalent for the corpus we
  // can compile right now).  When const_expr_val'd init folds fail,
  // fall back to vla_of.
  Token *dim_start = tok;
  Node *expr_n = cond_expr(&tok, tok);
  add_type(expr_n);
  tok = skip(tok, "]");
  Type *rest_ty = type_suffix(rest, tok, ty);

  int64_t len;
  if (try_eval_node(expr_n, &len))
    return array_of(rest_ty, (long)len);

  Type *vla = vla_of(rest_ty, expr_n);
  vla->vla_dim_tok = dim_start;
  return vla;
}

//
// global_variable (04a_decl.md §I.6).  No initializer support yet —
// `int x = 5;` will hit the error_tok arm.
//

// try_eval_addr_v2 — fold a compile-time pointer-typed expression
// to a (label, addend) pair so it can become a relocation in static
// init.  Subset modeled after canonical eval2 (parse.c:1870), enough
// for the patterns real-world headers / sqlite / linux use:
//
//   &gvar                    -> &gvar.name + 0
//   &gvar->field   /  &gvar.field
//                            -> &gvar.name + member offset
//   &arr[N]   /  arr+N   /  arr-N   (arr is a global array)
//                            -> &arr.name + N*sizeof(*arr)
//   func                     -> &func.name + 0   (function decay)
//   ND_CAST(inner)           -> recurse, ignore the cast
//   ND_ADD(addr, const_int)  -> addr.label, addr.addend + const
//   ND_SUB(addr, const_int)  -> addr.label, addr.addend - const
//
// Returns true and writes *out_label / *out_addend on success.
// Returns false otherwise; caller falls back to integer fold.
static bool try_eval_addr_v2(Node *e, char **out_label, int64_t *out_addend) {
  if (!e) return false;
  add_type(e);
  switch (e->kind) {
  case ND_ADDR: {
    Node *inner = e->lhs;
    if (!inner) return false;
    if (inner->kind == ND_VAR && inner->var && !inner->var->is_local) {
      *out_label = inner->var->name;
      *out_addend = 0;
      return true;
    }
    if (inner->kind == ND_DEREF) {
      // &*x == x
      return try_eval_addr_v2(inner->lhs, out_label, out_addend);
    }
    if (inner->kind == ND_MEMBER && inner->member &&
        inner->lhs && inner->lhs->kind == ND_VAR && inner->lhs->var &&
        !inner->lhs->var->is_local) {
      *out_label = inner->lhs->var->name;
      *out_addend = inner->member->offset;
      return true;
    }
    return false;
  }
  case ND_VAR:
    // Function or array decays to pointer.  For other gvars, taking
    // the rvalue isn't a static-init address.
    if (e->var && !e->var->is_local && e->var->ty &&
        (e->var->ty->kind == TY_FUNC || e->var->ty->kind == TY_ARRAY)) {
      *out_label = e->var->name;
      *out_addend = 0;
      return true;
    }
    return false;
  case ND_CAST:
    return try_eval_addr_v2(e->lhs, out_label, out_addend);
  case ND_ADD: {
    char *lbl = NULL;
    int64_t a = 0;
    int64_t k;
    if (try_eval_addr_v2(e->lhs, &lbl, &a) && try_eval_node(e->rhs, &k)) {
      *out_label = lbl;
      *out_addend = a + k;
      return true;
    }
    if (try_eval_addr_v2(e->rhs, &lbl, &a) && try_eval_node(e->lhs, &k)) {
      *out_label = lbl;
      *out_addend = a + k;
      return true;
    }
    return false;
  }
  case ND_SUB: {
    char *lbl = NULL;
    int64_t a = 0;
    int64_t k;
    if (try_eval_addr_v2(e->lhs, &lbl, &a) && try_eval_node(e->rhs, &k)) {
      *out_label = lbl;
      *out_addend = a - k;
      return true;
    }
    return false;
  }
  case ND_COMMA:
    return try_eval_addr_v2(e->rhs, out_label, out_addend);
  default:
    return false;
  }
}

// concat_adjacent_strings — if `tok` is TK_STR followed by another
// TK_STR (and so on), merge the chain into a single TK_STR token in
// place: tok takes the merged bytes + type, and tok->next jumps past
// the trailing string in the chain.  Returns true if any merge
// happened.  Mirrors canonical parse.c:3790.
//
// Used by the gvar / lvar initializer paths that consume a single
// TK_STR — without this they choke on `char *p = "foo" "bar";` and
// every adjacent-string init in real-world headers.
static bool concat_adjacent_strings(Token *tok) {
  if (tok->kind != TK_STR || tok->next->kind != TK_STR)
    return false;
  int total_len = 0;
  for (Token *t = tok; t->kind == TK_STR; t = t->next)
    total_len += t->ty->array_len - 1;
  total_len++;  // final NUL
  char *buf = calloc_checked(1, total_len);
  int offset = 0;
  Token *t = tok;
  while (t->kind == TK_STR) {
    int len = t->ty->array_len - 1;
    memcpy(buf + offset, t->str, len);
    offset += len;
    t = t->next;
  }
  buf[offset] = '\0';
  tok->ty = array_of(ty_char, total_len);
  tok->str = buf;
  tok->next = t;
  return true;
}

// parse_gvar_initializer — consumes `= initializer` for an Obj that
// already lives in the globals chain.  Subset (matches the spec's
// gvar paths in 04d):
//   - char[] = "literal":   bytes copied; incomplete len resolved.
//   - char *p = "literal":  anon gvar + Relocation.
//   - T arr[] of integer = { c1, c2, ... }: packed bytes.
//   - T arr[] of pointer = { "s1", "s2", 0, ... }: string anon
//     gvars + relocations + zero slots.
//   - scalar T x = const-expr: folded little-endian bytes.
// Other forms surface "this global initializer form not yet
// implemented" diagnostics.
static Token *parse_gvar_initializer(Token *tok, Obj *var) {
  Token *eq = tok;
  tok = tok->next;

  concat_adjacent_strings(tok);
  if (tok->kind == TK_STR && var->ty->kind == TY_ARRAY &&
      var->ty->base->kind == TY_CHAR) {
    long s = tok->ty->array_len;
    if (var->ty->array_len < 0) {
      var->ty = array_of(var->ty->base, s);
      var->align = var->ty->align;
    }
    long n = var->ty->array_len < s ? var->ty->array_len : s;
    char *buf = calloc_checked(1, var->ty->size);
    for (long i = 0; i < n; i++) buf[i] = tok->str[i];
    var->init_data = buf;
    var->init_data_size = var->ty->size;
    return tok->next;
  }
  if (tok->kind == TK_STR && var->ty->kind == TY_PTR &&
      var->ty->base->kind == TY_CHAR) {
    Obj *anon = new_anon_gvar(tok->ty);
    anon->init_data = tok->str;
    anon->init_data_size = tok->ty->size;
    Relocation *r = calloc_checked(1, sizeof(Relocation));
    r->offset = 0;
    r->label = &anon->name;
    var->rel = r;
    var->init_data = calloc_checked(1, var->ty->size);
    var->init_data_size = var->ty->size;
    return tok->next;
  }
  if (equal(tok, "{") && var->ty->kind == TY_ARRAY &&
      var->ty->base->kind == TY_PTR) {
    tok = tok->next;
    long elem_sz = var->ty->base->size;
    long count = 0;
    long max_elems = 4096;
    int64_t *vals = calloc_checked(max_elems, sizeof(int64_t));
    char **rels = calloc_checked(max_elems, sizeof(char *));
    bool first2 = true;
    while (!equal(tok, "}")) {
      if (!first2) tok = skip(tok, ",");
      first2 = false;
      if (equal(tok, "}")) break;
      if (count >= max_elems) error_tok(tok, "too many initializer elements");
      concat_adjacent_strings(tok);
      if (tok->kind == TK_STR) {
        Obj *anon = new_anon_gvar(tok->ty);
        anon->init_data = tok->str;
        anon->init_data_size = tok->ty->size;
        rels[count] = anon->name;
        tok = tok->next;
      } else {
        // Try parsing as an expression first.  Detect any
        // static-address-of pattern (`&gvar`, `arr+k`, `(T*)&gvar`,
        // `(T*)"literal"`, etc.) via try_eval_addr_v2 so the relocation
        // captures the gvar + offset.  Fall back to const_expr_val for
        // plain integer pointer values.
        Token *probe = tok;
        Node *e = assign(&probe, tok);
        add_type(e);
        char *lbl = NULL;
        int64_t addend = 0;
        if (try_eval_addr_v2(e, &lbl, &addend)) {
          rels[count] = lbl;
          // We can't represent a non-zero addend in this lightweight
          // (rels[] + vals[]) ladder; use vals[count] for the addend
          // bytes that get OR'd in below.  Most real-world cases have
          // addend == 0 (plain pointer to a gvar / string literal).
          vals[count] = addend;
          tok = probe;
        } else {
          vals[count] = const_expr_val(&tok, tok);
        }
      }
      count++;
    }
    tok = skip(tok, "}");
    if (var->ty->array_len < 0) {
      var->ty = array_of(var->ty->base, count);
      var->align = var->ty->align;
    }
    long sz = var->ty->size;
    char *buf = calloc_checked(1, sz);
    Relocation rh = {0};
    Relocation *rcur = &rh;
    for (long i = 0; i < count && i * elem_sz < sz; i++) {
      if (rels[i]) {
        Relocation *r = calloc_checked(1, sizeof(Relocation));
        r->offset = (int)(i * elem_sz);
        r->addend = vals[i];
        for (Obj *o = globals; o; o = o->next) {
          if (!strcmp(o->name, rels[i])) { r->label = &o->name; break; }
        }
        rcur = rcur->next = r;
      } else {
        int64_t v = vals[i];
        for (int b = 0; b < elem_sz; b++)
          buf[i * elem_sz + b] = (v >> (b * 8)) & 0xff;
      }
    }
    var->init_data = buf;
    var->init_data_size = sz;
    var->rel = rh.next;
    return tok;
  }
  // Struct gvar init: brace-init of struct fields directly.  Subset:
  // each field is a string literal (becomes Relocation), pointer
  // (relocation), or integer/bool const-expr.  Nested aggregates
  // not yet handled.
  if (equal(tok, "{") && var->ty->kind == TY_STRUCT) {
    Type *ty = var->ty;
    long sz = ty->size;
    char *buf = calloc_checked(1, sz);
    Relocation rh = {0};
    Relocation *rcur = &rh;
    tok = tok->next;
    Member *m = ty->members;
    bool first = true;
    while (!equal(tok, "}")) {
      if (!first) tok = skip(tok, ",");
      first = false;
      if (equal(tok, "}")) break;
      if (equal(tok, ".")) {
        tok = tok->next;
        if (tok->kind != TK_IDENT) error_tok(tok, "expected member name");
        Member *target = find_member(ty, tok);
        if (!target) error_tok(tok, "no such member");
        tok = skip(tok->next, "=");
        m = target;
      }
      if (!m) error_tok(tok, "excess elements in struct initializer");
      concat_adjacent_strings(tok);
      if (m->ty->kind == TY_PTR && tok->kind == TK_STR) {
        Obj *anon = new_anon_gvar(tok->ty);
        anon->init_data = tok->str;
        anon->init_data_size = tok->ty->size;
        Relocation *r = calloc_checked(1, sizeof(Relocation));
        r->offset = (int)m->offset;
        r->label = &anon->name;
        rcur = rcur->next = r;
        tok = tok->next;
      } else if (m->ty->kind == TY_ARRAY && m->ty->base->kind == TY_CHAR &&
                 tok->kind == TK_STR) {
        // Direct char-array init from string literal.
        long s = tok->ty->array_len;
        long n = m->ty->array_len > 0 && m->ty->array_len < s ? m->ty->array_len : s;
        for (long i = 0; i < n && (m->offset + i) < sz; i++)
          buf[m->offset + i] = tok->str[i];
        tok = tok->next;
      } else if (equal(tok, "{") || m->ty->kind == TY_STRUCT ||
                 m->ty->kind == TY_UNION || m->ty->kind == TY_ARRAY ||
                 m->ty->kind == TY_PTR ||
                 is_flonum(m->ty)) {
        // Nested aggregate / pointer / float field — recurse.
        tok = gvar_subinit_recursive(tok, m->ty, buf, m->offset, sz, &rcur);
      } else {
        int64_t v = const_expr_val(&tok, tok);
        for (int b = 0; b < (int)m->ty->size; b++)
          buf[m->offset + b] = (v >> (b * 8)) & 0xff;
      }
      m = m->next;
    }
    tok = skip(tok, "}");
    var->init_data = buf;
    var->init_data_size = sz;
    var->rel = rh.next;
    return tok;
  }

  // Union gvar init: only the first member is initialized; all
  // others share its storage.  Subset: integer first member.
  if (equal(tok, "{") && var->ty->kind == TY_UNION) {
    Type *ty = var->ty;
    long sz = ty->size;
    char *buf = calloc_checked(1, sz);
    tok = tok->next;
    Member *m = ty->members;
    if (!equal(tok, "}") && m) {
      concat_adjacent_strings(tok);
      if (m->ty->kind == TY_PTR && tok->kind == TK_STR) {
        Obj *anon = new_anon_gvar(tok->ty);
        anon->init_data = tok->str;
        anon->init_data_size = tok->ty->size;
        Relocation *r = calloc_checked(1, sizeof(Relocation));
        r->offset = 0;
        r->label = &anon->name;
        var->rel = r;
        tok = tok->next;
      } else {
        int64_t v = const_expr_val(&tok, tok);
        for (int b = 0; b < (int)m->ty->size && b < (int)sz; b++)
          buf[b] = (v >> (b * 8)) & 0xff;
      }
    }
    // Trailing comma allowed.
    if (equal(tok, ",")) tok = tok->next;
    tok = skip(tok, "}");
    var->init_data = buf;
    var->init_data_size = sz;
    return tok;
  }

  // Array of struct: brace contains brace per element; each inner
  // brace folds the struct fields.  Subset (matches our table use):
  // each field is either a string literal (becomes Relocation),
  // pointer-to-string-literal, or integer/bool const-expr.
  if (equal(tok, "{") && var->ty->kind == TY_ARRAY &&
      var->ty->base->kind == TY_STRUCT) {
    Type *elem_ty = var->ty->base;
    long elem_sz = elem_ty->size;
    tok = tok->next;
    long count = 0;
    long max_elems = 4096;
    char *buf = NULL;
    long alloc_sz = 0;
    Relocation rh = {0};
    Relocation *rcur = &rh;
    bool first2 = true;
    while (!equal(tok, "}")) {
      if (!first2) tok = skip(tok, ",");
      first2 = false;
      if (equal(tok, "}")) break;
      if (count >= max_elems) error_tok(tok, "too many initializer elements");
      tok = skip(tok, "{");
      // Ensure buf has room for this element.
      long need = (count + 1) * elem_sz;
      if (need > alloc_sz) {
        // Grow.
        long new_sz = alloc_sz ? alloc_sz * 2 : 64;
        while (new_sz < need) new_sz *= 2;
        char *nb = calloc_checked(1, new_sz);
        if (buf) for (long i = 0; i < alloc_sz; i++) nb[i] = buf[i];
        buf = nb;
        alloc_sz = new_sz;
      }
      Member *m = elem_ty->members;
      bool fst = true;
      while (!equal(tok, "}")) {
        if (!fst) tok = skip(tok, ",");
        fst = false;
        if (equal(tok, "}")) break;
        if (!m) error_tok(tok, "excess elements in struct initializer");
        long base_off = count * elem_sz + m->offset;
        concat_adjacent_strings(tok);
        if (m->ty->kind == TY_PTR && tok->kind == TK_STR) {
          // Anon gvar + relocation.
          Obj *anon = new_anon_gvar(tok->ty);
          anon->init_data = tok->str;
          anon->init_data_size = tok->ty->size;
          tok = tok->next;
          Relocation *r = calloc_checked(1, sizeof(Relocation));
          r->offset = (int)base_off;
          r->label = &anon->name;
          rcur = rcur->next = r;
        } else if (equal(tok, "{") || m->ty->kind == TY_STRUCT ||
                   m->ty->kind == TY_UNION || m->ty->kind == TY_ARRAY ||
                   m->ty->kind == TY_PTR ||
                   is_flonum(m->ty)) {
          // Aggregate / pointer / float member -> route via the recursive
          // helper (it handles relocations, flonum, char-array, nested
          // braces, &gvar/&arr[N]/etc. via try_eval_addr_v2).
          tok = gvar_subinit_recursive(tok, m->ty, buf, base_off,
                                        alloc_sz, &rcur);
        } else {
          int64_t v = const_expr_val(&tok, tok);
          for (int b = 0; b < (int)m->ty->size; b++)
            buf[base_off + b] = (v >> (b * 8)) & 0xff;
        }
        m = m->next;
      }
      tok = skip(tok, "}");
      count++;
    }
    tok = skip(tok, "}");
    if (var->ty->array_len < 0) {
      var->ty = array_of(elem_ty, count);
      var->align = var->ty->align;
    }
    long sz = var->ty->size;
    if (sz > alloc_sz) {
      char *nb = calloc_checked(1, sz);
      if (buf) for (long i = 0; i < alloc_sz; i++) nb[i] = buf[i];
      buf = nb;
    }
    var->init_data = buf;
    var->init_data_size = sz;
    var->rel = rh.next;
    return tok;
  }

  if (equal(tok, "{") && var->ty->kind == TY_ARRAY && is_integer(var->ty->base)) {
    tok = tok->next;
    long elem_sz = var->ty->base->size;
    long count = 0;
    long cap = 1024;
    int64_t *vals = calloc_checked(cap, sizeof(int64_t));
    bool first2 = true;
    while (!equal(tok, "}")) {
      if (!first2) tok = skip(tok, ",");
      first2 = false;
      if (equal(tok, "}")) break;
      if (count >= cap) {
        long new_cap = cap * 2;
        int64_t *nvals = calloc_checked(new_cap, sizeof(int64_t));
        for (long i = 0; i < cap; i++) nvals[i] = vals[i];
        vals = nvals;
        cap = new_cap;
      }
      vals[count++] = const_expr_val(&tok, tok);
    }
    tok = skip(tok, "}");
    if (var->ty->array_len < 0) {
      var->ty = array_of(var->ty->base, count);
      var->align = var->ty->align;
    }
    long sz = var->ty->size;
    char *buf = calloc_checked(1, sz);
    for (long i = 0; i < count && i * elem_sz < sz; i++) {
      int64_t v = vals[i];
      for (int b = 0; b < elem_sz; b++)
        buf[i * elem_sz + b] = (v >> (b * 8)) & 0xff;
    }
    var->init_data = buf;
    var->init_data_size = sz;
    return tok;
  }
  // Array of float/double scalar — element-wise const-expr fold via
  // try_eval_double_v2.  TY_LDOUBLE on Apple ARM64 has size 8 (== double).
  if (equal(tok, "{") && var->ty->kind == TY_ARRAY && is_flonum(var->ty->base)) {
    tok = tok->next;
    long elem_sz = var->ty->base->size;
    long count = 0;
    long max_elems = 4096;
    double *vals = calloc_checked(max_elems, sizeof(double));
    bool first2 = true;
    while (!equal(tok, "}")) {
      if (!first2) tok = skip(tok, ",");
      first2 = false;
      if (equal(tok, "}")) break;
      if (count >= max_elems) error_tok(tok, "too many initializer elements");
      Node *e = assign(&tok, tok);
      add_type(e);
      double dv;
      if (!try_eval_double_v2(e, &dv))
        error_tok(eq, "parse_v2: array-of-float gvar init not const-foldable");
      vals[count++] = dv;
    }
    tok = skip(tok, "}");
    if (var->ty->array_len < 0) {
      var->ty = array_of(var->ty->base, count);
      var->align = var->ty->align;
    }
    long sz = var->ty->size;
    char *buf = calloc_checked(1, sz);
    for (long i = 0; i < count && i * elem_sz < sz; i++) {
      if (var->ty->base->kind == TY_FLOAT) {
        float f = (float)vals[i];
        memcpy(buf + i * elem_sz, &f, 4);
      } else {
        memcpy(buf + i * elem_sz, &vals[i], elem_sz < 8 ? (size_t)elem_sz : 8);
      }
    }
    var->init_data = buf;
    var->init_data_size = sz;
    return tok;
  }
  // Scalar float / double / long double (long double is double-sized
  // on Apple ARM64 per Phase 3 Q10).
  if (is_flonum(var->ty)) {
    Node *e = assign(&tok, tok);
    add_type(e);
    double dv;
    if (!try_eval_double_v2(e, &dv))
      error_tok(eq, "parse_v2: scalar float gvar init not const-foldable");
    long sz = var->ty->size;
    char *buf = calloc_checked(1, sz);
    if (var->ty->kind == TY_FLOAT) {
      float f = (float)dv;
      memcpy(buf, &f, 4);
    } else {
      memcpy(buf, &dv, sz < 8 ? (size_t)sz : 8);
    }
    var->init_data = buf;
    var->init_data_size = sz;
    return tok;
  }
  if (is_integer(var->ty) || var->ty->kind == TY_PTR) {
    // Pointer special path: detect `&gvar` (ND_ADDR of ND_VAR
    // where the var is a global) and emit a Relocation rather
    // than trying to fold an address as an integer.
    if (var->ty->kind == TY_PTR) {
      Token *probe = tok;
      Node *e = assign(&probe, tok);
      add_type(e);
      char *lbl = NULL;
      int64_t addend = 0;
      if (try_eval_addr_v2(e, &lbl, &addend)) {
        Relocation *r = calloc_checked(1, sizeof(Relocation));
        r->offset = 0;
        // Find the gvar by name to alias its name pointer (the Relocation
        // emitter dereferences `r->label`, so it must outlive this call).
        for (Obj *o = globals; o; o = o->next) {
          if (!strcmp(o->name, lbl)) { r->label = &o->name; break; }
        }
        if (!r->label) {
          // Fall through to integer fold; the relocation target wasn't
          // a known global (could be a forward decl that hasn't been
          // declared yet — rare; const_expr_val will then error cleanly).
        } else {
          r->addend = addend;
          var->rel = r;
          var->init_data = calloc_checked(1, var->ty->size);
          var->init_data_size = var->ty->size;
          return probe;
        }
      }
    }
    int64_t v = const_expr_val(&tok, tok);
    long sz = var->ty->size;
    char *buf = calloc_checked(1, sz);
    for (long b = 0; b < sz; b++)
      buf[b] = (v >> (b * 8)) & 0xff;
    var->init_data = buf;
    var->init_data_size = sz;
    return tok;
  }
  // Generic aggregate fallback — multi-dim arrays, deeply nested
  // structs, anything not matched by the specialized arms above.
  // Route through gvar_subinit_recursive.
  if (equal(tok, "{") &&
      (var->ty->kind == TY_ARRAY || var->ty->kind == TY_STRUCT ||
       var->ty->kind == TY_UNION)) {
    long sz = var->ty->size;
    if (sz < 0) sz = 0;
    char *buf = calloc_checked(1, sz > 0 ? sz : 1);
    Relocation rh = {0};
    Relocation *rcur = &rh;
    Token *after = gvar_subinit_recursive(tok, var->ty, buf, 0, sz, &rcur);
    var->init_data = buf;
    var->init_data_size = sz;
    var->rel = rh.next;
    return after;
  }
  error_tok(eq, "parse_v2: this global initializer form not yet implemented");
}

static Token *global_variable(Token *tok, Type *basety, Type *first_ty,
                               VarAttr *attr) {
  Type *ty = first_ty;
  bool first = true;
  for (;;) {
    if (!first) {
      // Subsequent declarator after `,`.
      ty = declarator(&tok, tok, basety);
    }
    first = false;

    if (!ty->name)
      error_tok(tok, "parse_v2: global variable requires a name");

    char *name = strndup_checked(ty->name->loc, ty->name->len);
    Obj *var = new_gvar(name, ty);
    var->is_static = attr->is_static;
    var->is_tls    = attr->is_tls;
    var->is_definition = !attr->is_extern;
    if (attr->align) var->align = attr->align;

    // Optional __asm__ label and __attribute__ post-declarator.
    tok = skip_asm_label(tok);
    if (equal(tok, "__attribute__"))
      tok = attribute_list(tok->next, var->ty, attr);

    if (equal(tok, "=")) {
      tok = parse_gvar_initializer(tok, var);
    }

    if (equal(tok, ",")) { tok = tok->next; continue; }
    return skip(tok, ";");
  }
}

//
// function_declaration — non-defining function declaration loop.
// Per §I.2 step 4 third bullet: comma-separated declarators, each
// becomes a non-defining Obj with optional asm/attribute, then `;`.
//

static Token *function_declaration(Token *tok, Type *basety, Type *first_ty,
                                    VarAttr *attr) {
  Type *ty = first_ty;
  bool first = true;
  for (;;) {
    if (!first) {
      ty = declarator(&tok, tok, basety);
      if (ty->kind != TY_FUNC)
        error_tok(tok, "parse_v2: mixed function/variable declaration list");
    }
    first = false;

    if (!ty->name)
      error_tok(tok, "parse_v2: function declaration requires a name");

    char *name = strndup_checked(ty->name->loc, ty->name->len);
    Obj *fn = new_gvar(name, ty);
    fn->is_function = true;
    fn->is_definition = false;
    fn->is_static = attr->is_static;
    fn->is_inline = attr->is_inline;

    tok = skip_asm_label(tok);
    if (equal(tok, "__attribute__"))
      tok = attribute_list(tok->next, fn->ty, attr);

    if (equal(tok, ",")) { tok = tok->next; continue; }
    return skip(tok, ";");
  }
}

// skip_attribute — minimal stray-attribute consumer.  Walks past
// `__attribute__ ( ( ... ) )` with brace-balanced inner content.
// Real attribute_list (04a §G) parses content; this just skips.
static Token *skip_attribute(Token *tok) {
  tok = skip(tok, "__attribute__");
  tok = skip(tok, "(");
  tok = skip(tok, "(");
  int depth = 1;
  while (depth > 0 && tok->kind != TK_EOF) {
    if (equal(tok, "(")) depth++;
    else if (equal(tok, ")")) depth--;
    if (depth > 0) tok = tok->next;
  }
  tok = skip(tok, ")");
  tok = skip(tok, ")");
  return tok;
}

// skip_static_assert — file-scope _Static_assert.  Per the spec
// note, block-scope variant is silently dropped (in-block branch
// handled separately at compound_stmt).  Form:
//   _Static_assert ( const-expr [ , string-literal ] ) ;
static Token *skip_static_assert(Token *tok) {
  Token *kw = tok;
  tok = tok->next;
  tok = skip(tok, "(");
  int64_t v = const_expr_val(&tok, tok);
  // Optional message string.
  char *msg = NULL;
  if (equal(tok, ",")) {
    tok = tok->next;
    if (tok->kind != TK_STR)
      error_tok(tok, "expected string literal in _Static_assert message");
    msg = tok->str;
    tok = tok->next;
  }
  tok = skip(tok, ")");
  tok = skip(tok, ";");
  if (!v)
    error_tok(kw, "static assertion failed: %s", msg ? msg : "no message");
  return tok;
}

//
// Statement zone (04c_stmt.md).
//

// compound_stmt — `{` already consumed by caller.  Parses
// declarations and statements until `}` and returns an ND_BLOCK.
static Node *compound_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_BLOCK, tok);
  Node head = {0};
  Node *cur = &head;

  enter_scope();
  while (!equal(tok, "}")) {
    // Block-scope _Static_assert (04c §B.3 step 2).  Spec says the
    // in-block form silently succeeds; we route through the same
    // skip_static_assert as file scope so failures still error.
    if (equal(tok, "_Static_assert") || equal(tok, "static_assert")) {
      tok = skip_static_assert(tok);
      continue;
    }

    // Declaration: is_typename and the next token isn't `:` (which
    // would make it a labeled statement using a typedef name).
    if (is_typename(tok) && !equal(tok->next, ":")) {
      VarAttr attr = {0};
      Type *basety = declspec(&tok, tok, &attr);
      if (attr.is_typedef) {
        tok = parse_typedef(tok, basety);
        continue;
      }
      cur = cur->next = declaration(&tok, tok, basety, &attr);
      add_type(cur);
      continue;
    }
    cur = cur->next = stmt(&tok, tok);
    add_type(cur);
  }
  leave_scope();

  *rest = skip(tok, "}");
  node->body = head.next;
  return node;
}

// stmt — statement dispatch.  Currently handles only what the
// vertical slice needs: `return` and block.  Other branches land
// incrementally per 04c.
static Node *stmt(Token **rest, Token *tok) {
  // `return` ;
  // `return` expr ;
  if (equal(tok, "return")) {
    if (equal(tok->next, ";")) {
      Node *node = new_unary(ND_RETURN, NULL, tok);
      *rest = tok->next->next;
      return node;
    }
    Token *ret_tok = tok;
    Node *e = expr(&tok, tok->next);
    add_type(e);
    Type *ret_ty = current_fn ? current_fn->ty->return_ty : ty_int;
    Node *node = new_unary(ND_RETURN, new_cast(e, ret_ty), ret_tok);
    *rest = skip(tok, ";");
    return node;
  }

  // if / else (04c §C.1).  No DCE yet — that's a parser-side
  // optimization that depends on contains_label + const-folding
  // and lands once try_eval_node is real.
  if (equal(tok, "if")) {
    Token *if_tok = tok;
    tok = skip(tok->next, "(");
    Node *cond_n = expr(&tok, tok);
    tok = skip(tok, ")");
    Node *then_n = stmt(&tok, tok);
    Node *els_n = NULL;
    if (equal(tok, "else"))
      els_n = stmt(&tok, tok->next);
    Node *node = new_node(ND_IF, if_tok);
    node->cond = cond_n;
    node->then = then_n;
    node->els = els_n;
    *rest = tok;
    return node;
  }

  // while (04c §D.1).
  if (equal(tok, "while")) {
    Token *w_tok = tok;
    tok = skip(tok->next, "(");
    Node *cond_n = expr(&tok, tok);
    tok = skip(tok, ")");

    Node *node = new_node(ND_FOR, w_tok);
    node->cond = cond_n;
    node->unique_label = new_unique_name();
    node->cont_label = new_unique_name();

    char *saved_brk = brk_label, *saved_cont = cont_label;
    brk_label = node->unique_label;
    cont_label = node->cont_label;
    node->then = stmt(&tok, tok);
    brk_label = saved_brk;
    cont_label = saved_cont;

    *rest = tok;
    return node;
  }

  // do { ... } while ( ... ); (04c §D.2).
  if (equal(tok, "do")) {
    Token *do_tok = tok;
    Node *node = new_node(ND_DO, do_tok);
    node->unique_label = new_unique_name();
    node->cont_label = new_unique_name();

    char *saved_brk = brk_label, *saved_cont = cont_label;
    brk_label = node->unique_label;
    cont_label = node->cont_label;
    node->then = stmt(&tok, tok->next);
    brk_label = saved_brk;
    cont_label = saved_cont;

    tok = skip(tok, "while");
    tok = skip(tok, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    *rest = skip(tok, ";");
    return node;
  }

  // for (init; cond; step) stmt (04c §D.3).
  if (equal(tok, "for")) {
    Token *for_tok = tok;
    tok = skip(tok->next, "(");

    Node *node = new_node(ND_FOR, for_tok);
    node->unique_label = new_unique_name();
    node->cont_label = new_unique_name();

    enter_scope();
    char *saved_brk = brk_label, *saved_cont = cont_label;
    brk_label = node->unique_label;
    cont_label = node->cont_label;

    // Init: declaration | expr-stmt | `;`.
    if (equal(tok, ";")) {
      tok = tok->next;
    } else if (is_typename(tok)) {
      VarAttr attr = {0};
      Type *basety = declspec(&tok, tok, &attr);
      node->init = declaration(&tok, tok, basety, &attr);
    } else {
      Node *e = expr(&tok, tok);
      node->init = new_unary(ND_EXPR_STMT, e, for_tok);
      tok = skip(tok, ";");
    }

    if (!equal(tok, ";"))
      node->cond = expr(&tok, tok);
    tok = skip(tok, ";");

    if (!equal(tok, ")"))
      node->inc = expr(&tok, tok);
    tok = skip(tok, ")");

    node->then = stmt(&tok, tok);

    brk_label = saved_brk;
    cont_label = saved_cont;
    leave_scope();

    *rest = tok;
    return node;
  }

  // asm / __asm__ / __asm  ( template [: outputs : inputs : clobbers : labels] ) ;
  // 04c §E.6 minimal skeleton: capture the (optionally adjacent-
  // concatenated) string template, brace-balanced-skip everything
  // else, emit ND_ASM with no operands.  Files with operand-bearing
  // inline asm will fail at the assembler step — this is a known
  // gap until we wire the full operand machinery.
  if (equal(tok, "asm") || equal(tok, "__asm__") || equal(tok, "__asm")) {
    Token *asm_tok = tok;
    tok = tok->next;
    while (equal(tok, "volatile") || equal(tok, "__volatile__") ||
           equal(tok, "inline") || equal(tok, "goto"))
      tok = tok->next;
    tok = skip(tok, "(");

    char *tmpl = "";
    if (tok->kind == TK_STR) {
      tmpl = tok->str;
      tok = tok->next;
      while (tok->kind == TK_STR) {
        size_t la = strlen(tmpl);
        size_t lb = strlen(tok->str);
        char *cat = calloc_checked(1, la + lb + 1);
        memcpy(cat, tmpl, la);
        memcpy(cat + la, tok->str, lb);
        tmpl = cat;
        tok = tok->next;
      }
    }

    // Skip the rest of the parenthesized body brace-balanced.
    int depth = 1;
    while (depth > 0 && tok->kind != TK_EOF) {
      if (equal(tok, "(")) depth++;
      else if (equal(tok, ")")) { depth--; if (depth == 0) break; }
      tok = tok->next;
    }
    tok = skip(tok, ")");
    tok = skip(tok, ";");

    Node *node = new_node(ND_ASM, asm_tok);
    node->asm_str = tmpl;
    *rest = tok;
    return node;
  }

  // switch (cond) stmt (04c §C.2).
  if (equal(tok, "switch")) {
    Token *sw_tok = tok;
    tok = skip(tok->next, "(");
    Node *cond_n = expr(&tok, tok);
    tok = skip(tok, ")");

    Node *node = new_node(ND_SWITCH, sw_tok);
    node->cond = cond_n;
    node->unique_label = new_unique_name();

    Node *saved_switch = current_switch;
    char *saved_brk = brk_label;
    current_switch = node;
    brk_label = node->unique_label;
    node->then = stmt(&tok, tok);
    current_switch = saved_switch;
    brk_label = saved_brk;

    *rest = tok;
    return node;
  }

  // case CONST [...CONST] : stmt (04c §C.3).
  if (equal(tok, "case")) {
    if (!current_switch)
      error_tok(tok, "stray case");
    Token *case_tok = tok;
    int64_t begin = const_expr_val(&tok, tok->next);
    int64_t end = begin;
    if (equal(tok, "...")) {
      end = const_expr_val(&tok, tok->next);
    }
    tok = skip(tok, ":");

    Node *node = new_node(ND_CASE, case_tok);
    node->label = new_unique_name();
    node->begin = (long)begin;
    node->end = (long)end;
    node->lhs = stmt(&tok, tok);

    node->case_next = current_switch->case_next;
    current_switch->case_next = node;

    *rest = tok;
    return node;
  }

  // default : stmt (04c §C.3).
  if (equal(tok, "default")) {
    if (!current_switch)
      error_tok(tok, "stray default");
    Token *def_tok = tok;
    tok = skip(tok->next, ":");

    Node *node = new_node(ND_CASE, def_tok);
    node->label = new_unique_name();
    node->lhs = stmt(&tok, tok);
    current_switch->default_case = node;

    *rest = tok;
    return node;
  }

  // goto (04c §E.1).  Direct: `goto IDENT;`.  Computed: `goto * expr;`.
  if (equal(tok, "goto")) {
    Token *g_tok = tok;
    // Computed form.
    if (equal(tok->next, "*")) {
      Node *e = expr(&tok, tok->next->next);
      Node *node = new_unary(ND_GOTO_EXPR, e, g_tok);
      *rest = skip(tok, ";");
      return node;
    }
    // Direct form.
    if (tok->next->kind != TK_IDENT)
      error_tok(tok->next, "expected label");
    Node *node = new_node(ND_GOTO, g_tok);
    node->label = strndup_checked(tok->next->loc, tok->next->len);
    node->goto_next = gotos;
    gotos = node;
    *rest = skip(tok->next->next, ";");
    return node;
  }

  // break (04c §E.2).
  if (equal(tok, "break")) {
    if (!brk_label)
      error_tok(tok, "stray break");
    Node *node = new_node(ND_GOTO, tok);
    node->unique_label = brk_label;
    *rest = skip(tok->next, ";");
    return node;
  }

  // continue (04c §E.3).
  if (equal(tok, "continue")) {
    if (!cont_label)
      error_tok(tok, "stray continue");
    Node *node = new_node(ND_GOTO, tok);
    node->unique_label = cont_label;
    *rest = skip(tok->next, ";");
    return node;
  }

  // Labeled statement: `IDENT :` followed by stmt (04c §E.4).
  if (tok->kind == TK_IDENT && equal(tok->next, ":")) {
    Node *node = new_node(ND_LABEL, tok);
    node->label = strndup_checked(tok->loc, tok->len);
    node->unique_label = new_unique_name();
    tok = tok->next->next;
    // Permit trailing __attribute__((...)) per §E.4 step 3.
    if (equal(tok, "__attribute__"))
      tok = attribute_list(tok->next, NULL, NULL);
    node->lhs = stmt(&tok, tok);
    node->goto_next = labels;
    labels = node;
    *rest = tok;
    return node;
  }

  // Block.
  if (equal(tok, "{"))
    return compound_stmt(rest, tok->next);

  // Empty statement: `;`.
  if (equal(tok, ";")) {
    Node *node = new_node(ND_BLOCK, tok);
    *rest = tok->next;
    return node;
  }

  // Fall-through: expression statement (04c §E.7).
  Node *e = expr(&tok, tok);
  Node *node = new_unary(ND_EXPR_STMT, e, tok);
  *rest = skip(tok, ";");
  return node;
}

// declaration — local-declaration impl (04a §H).  Subset:
//   - Per-declarator loop: declarator, void check, name check.
//   - Storage class: extern (no storage) and normal new_lvar.
//     `static` local lifts to anon gvar — deferred until
//     gvar_initializer lands; flagged as not implemented if seen.
//   - Initializer: scalar `T x = expr;` only — array/struct/brace
//     forms (lvar_initializer) deferred until 04d lands.
//   - Returns ND_BLOCK whose body is the chain of init statements.
static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr) {
  Node head = {0};
  Node *cur = &head;
  bool first = true;

  while (!equal(tok, ";")) {
    if (!first)
      tok = skip(tok, ",");
    first = false;

    Type *ty = declarator(&tok, tok, basety);
    if (ty->kind == TY_VOID) {
      if (attr->is_extern) {
        ty = copy_type(ty_char);
      } else {
        error_tok(tok, "variable declared void");
      }
    }
    if (!ty->name)
      error_tok(tok, "declarator requires a name");

    if (equal(tok, "__attribute__"))
      tok = attribute_list(tok->next, ty, attr);

    char *name = strndup_checked(ty->name->loc, ty->name->len);

    if (attr->is_static) {
      // Lift to anon file-scope gvar with static linkage.  The local
      // name is bound in scope so subsequent uses resolve via the
      // anon gvar.  04a §H.3.
      Obj *gv = new_anon_gvar(ty);
      gv->is_static = true;
      // Bind the user-visible name in the local scope.
      VarScope *vs = push_scope(name);
      vs->var = gv;
      if (attr->align) gv->align = attr->align;
      if (equal(tok, "="))
        tok = parse_gvar_initializer(tok, gv);
      continue;
    }

    if (attr->is_extern) {
      Obj *var = new_gvar(name, ty);
      var->is_definition = false;
      // No storage; no initializer permitted with extern + decl.
      if (equal(tok, "="))
        error_tok(tok, "parse_v2: extern with initializer not yet implemented");
      continue;
    }

    Obj *var = new_lvar(name, ty);
    if (attr->align)
      var->align = attr->align;

    if (equal(tok, "=")) {
      Token *eq = tok;
      tok = tok->next;

      concat_adjacent_strings(tok);
      // String literal initializing a char array.  04d §F.
      if (tok->kind == TK_STR && ty->kind == TY_ARRAY &&
          ty->base->kind == TY_CHAR) {
        // Memzero first (so trailing slots are zero per C11).
        Node *memzero = new_node(ND_MEMZERO, eq);
        memzero->var = var;
        Node *chain = memzero;

        // Resolve incomplete length from the string size (including NUL).
        long s = tok->ty->array_len;
        if (var->ty->array_len < 0) {
          var->ty = array_of(var->ty->base, s);
          ty = var->ty;
        }
        long n = ty->array_len < s ? ty->array_len : s;
        for (long i = 0; i < n; i++) {
          Node *vref = new_var_node(var, eq);
          Node *sum = new_add(vref, new_num(i, eq), eq);
          Node *deref = new_unary(ND_DEREF, sum, eq);
          Node *val = new_num((unsigned char)tok->str[i], eq);
          Node *as = new_binary(ND_ASSIGN, deref, val, eq);
          chain = new_binary(ND_COMMA, chain, as, eq);
        }
        cur = cur->next = new_unary(ND_EXPR_STMT, chain, eq);
        tok = tok->next;
        continue;
      }

      // Brace initializer for arrays of scalars (and zero-init `{}`).
      // Brace-elision and nested aggregates deferred — full
      // lvar_initializer machinery lives in §G of 04d_init.md.
      if (equal(tok, "{")) {
        Node *memzero = new_node(ND_MEMZERO, eq);
        memzero->var = var;
        Node *chain = memzero;
        tok = tok->next;

        if (ty->kind == TY_ARRAY) {
          long i = 0;
          long cap = ty->array_len;  // -1 for incomplete
          while (!equal(tok, "}")) {
            if (i > 0) tok = skip(tok, ",");
            if (equal(tok, "}")) break;
            // Nested brace, struct, or array element — recurse via the
            // offset-based helper.  (The simple-scalar fast path
            // remains below.)
            if (equal(tok, "{") || ty->base->kind == TY_STRUCT ||
                ty->base->kind == TY_UNION || ty->base->kind == TY_ARRAY) {
              chain = lvar_init_at_offset(&tok, tok, ty->base, var,
                                           i * ty->base->size, chain, eq);
              i++;
              if (cap >= 0 && i >= cap) {
                while (!equal(tok, "}") && !equal(tok, ",")) tok = tok->next;
              }
              continue;
            }
            Node *e = assign(&tok, tok);
            add_type(e);
            // For `int a[3] = {0}` we may have cap >=0 and i < cap.
            // For incomplete arrays we accept arbitrary length.
            Node *vref = new_var_node(var, eq);
            Node *sum = new_add(vref, new_num(i, eq), eq);
            Node *deref = new_unary(ND_DEREF, sum, eq);
            Node *as = new_binary(ND_ASSIGN, deref, e, eq);
            chain = new_binary(ND_COMMA, chain, as, eq);
            i++;
            if (cap >= 0 && i >= cap) {
              // Excess elements would land here; consume up to `}`.
              while (!equal(tok, "}") && !equal(tok, ",")) tok = tok->next;
            }
          }
          tok = skip(tok, "}");
          if (cap < 0) {
            var->ty = array_of(ty->base, i);
            ty = var->ty;
          }
        } else if (ty->kind == TY_STRUCT) {
          // Struct brace init — member-by-member.  Supports the
          // `.name = expr` designator form.
          Member *m = ty->members;
          bool first2 = true;
          while (!equal(tok, "}")) {
            if (!first2) tok = skip(tok, ",");
            first2 = false;
            if (equal(tok, "}")) break;
            if (equal(tok, ".")) {
              tok = tok->next;
              if (tok->kind != TK_IDENT)
                error_tok(tok, "expected member name after `.`");
              Member *target = find_member(ty, tok);
              if (!target)
                error_tok(tok, "no such member");
              tok = skip(tok->next, "=");
              m = target;
            }
            if (!m)
              error_tok(tok, "excess elements in struct initializer");
            // Nested aggregate field — recurse via offset-based helper.
            if (equal(tok, "{") || m->ty->kind == TY_STRUCT ||
                m->ty->kind == TY_UNION || m->ty->kind == TY_ARRAY) {
              chain = lvar_init_at_offset(&tok, tok, m->ty, var,
                                           m->offset, chain, eq);
              m = m->next;
              continue;
            }
            Node *e = assign(&tok, tok);
            add_type(e);
            Node *vref = new_var_node(var, eq);
            Node *mn = new_unary(ND_MEMBER, vref, eq);
            mn->member = m;
            Node *as = new_binary(ND_ASSIGN, mn, e, eq);
            chain = new_binary(ND_COMMA, chain, as, eq);
            m = m->next;
          }
          tok = skip(tok, "}");
        } else {
          // `T x = {expr};` — single-expr brace form for scalars.
          if (!equal(tok, "}")) {
            Node *e = assign(&tok, tok);
            Node *vref = new_var_node(var, eq);
            // Add_type's ND_ASSIGN case injects the cast for non-aggregate types.
            Node *as = new_binary(ND_ASSIGN, vref, e, eq);
            chain = new_binary(ND_COMMA, chain, as, eq);
            // Trailing comma allowed.
            if (equal(tok, ",")) tok = tok->next;
          }
          tok = skip(tok, "}");
        }
        cur = cur->next = new_unary(ND_EXPR_STMT, chain, eq);
        continue;
      }

      // Scalar initializer fast path: `T x = expr;` lowers to an
      // ND_EXPR_STMT(ND_ASSIGN(var, expr)).
      Node *lhs = new_var_node(var, ty->name);
      Node *rhs = assign(&tok, tok);
      Node *expr_ = new_binary(ND_ASSIGN, lhs, rhs, eq);
      cur = cur->next = new_unary(ND_EXPR_STMT, expr_, eq);
    }
  }

  Node *node = new_node(ND_BLOCK, tok);
  node->body = head.next;
  *rest = skip(tok, ";");
  return node;
}

//
// Expression zone (04b_expr.md).  No vector/complex decomposition
// yet — those land in dedicated commits.  Compound assignment via
// to_assign is also deferred; only plain `=` is wired here.
//

// new_add — pointer-aware addition (04b §J.1).  Subset:
//   - num + num    → ND_ADD
//   - ptr + int    → ND_ADD with rhs scaled by sizeof(base)
//   - int + ptr    → swap and recurse
//   - ptr + ptr    → error
static Node *new_add(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  if (is_numeric(lhs->ty) && is_numeric(rhs->ty))
    return new_binary(ND_ADD, lhs, rhs, tok);

  if (lhs->ty->base && rhs->ty->base)
    error_tok(tok, "invalid operands (ptr + ptr)");

  // Canonicalize: ptr on lhs.
  if (!lhs->ty->base && rhs->ty->base) {
    Node *t = lhs; lhs = rhs; rhs = t;
  }

  // ptr + int — scale by element size.  Use new_long so the MUL
  // is computed in 64-bit (otherwise w-register truncation breaks
  // pointer arithmetic on real-world stack/heap addresses).
  long elem_size = lhs->ty->base->size;
  if (!elem_size) elem_size = 1;
  rhs = new_binary(ND_MUL, rhs, new_long(elem_size, tok), tok);
  return new_binary(ND_ADD, lhs, rhs, tok);
}

// new_sub — subset:
//   - num - num    → ND_SUB
//   - ptr - int    → ND_SUB with rhs scaled
//   - ptr - ptr    → byte diff / sizeof(base)  (long result)
static Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  if (is_numeric(lhs->ty) && is_numeric(rhs->ty))
    return new_binary(ND_SUB, lhs, rhs, tok);

  // ptr - int.  Force 64-bit scale (see new_add comment).
  if (lhs->ty->base && is_integer(rhs->ty)) {
    long elem_size = lhs->ty->base->size;
    if (!elem_size) elem_size = 1;
    rhs = new_binary(ND_MUL, rhs, new_long(elem_size, tok), tok);
    add_type(rhs);
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = lhs->ty;
    return node;
  }

  // ptr - ptr.
  if (lhs->ty->base && rhs->ty->base) {
    long elem_size = lhs->ty->base->size;
    if (!elem_size) elem_size = 1;
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = ty_long;
    return new_binary(ND_DIV, node, new_long(elem_size, tok), tok);
  }

  error_tok(tok, "invalid operands");
}

// expr → assign  (no comma operator yet)
static Node *expr(Token **rest, Token *tok) {
  Node *node = assign(&tok, tok);
  if (equal(tok, ",")) {
    Node *rhs = expr(&tok, tok->next);
    node = new_binary(ND_COMMA, node, rhs, tok);
  }
  *rest = tok;
  return node;
}

// assign → cond_expr ( = assign | op= assign )?
//
// Compound assignment per 04b §C.5: build a binary in the chosen
// kind and lower via to_assign.  Vector/complex/bitfield variants
// (§C.2/§C.3/§C.4) deferred — currently only the scalar to_assign
// path is wired.
static Node *assign(Token **rest, Token *tok) {
  Node *node = cond_expr(&tok, tok);

  if (equal(tok, "=")) {
    Token *eq = tok;
    Node *rhs = assign(&tok, tok->next);
    *rest = tok;
    return new_binary(ND_ASSIGN, node, rhs, eq);
  }

  // Compound assignment: pick the kind, build, lower.  The table
  // is at file scope (compound_op_table below) so parse_v2.c can
  // compile itself without array-of-struct gvar-init machinery.
  for (size_t i = 0; i < sizeof(compound_op_table) / sizeof(*compound_op_table); i++) {
    if (equal(tok, compound_op_table[i].op)) {
      Token *op = tok;
      Node *rhs = assign(&tok, tok->next);
      Node *binary;
      if (compound_op_table[i].kind == ND_ADD)
        binary = new_add(node, rhs, op);
      else if (compound_op_table[i].kind == ND_SUB)
        binary = new_sub(node, rhs, op);
      else
        binary = new_binary(compound_op_table[i].kind, node, rhs, op);
      *rest = tok;
      return to_assign(binary);
    }
  }

  *rest = tok;
  return node;
}

// cond_expr — ternary `?:` (04b §D).  Standard form only;
// GNU Elvis (`a ?: b`) is deferred — it lowers via tmp-var which
// requires new_lvar in expression context (works fine, just adds
// noise; folded into a follow-up commit).
static Node *cond_expr(Token **rest, Token *tok) {
  Node *node = logor(&tok, tok);
  if (!equal(tok, "?")) {
    *rest = tok;
    return node;
  }
  Token *q = tok;
  // GNU Elvis: `a ?: b` — defer.  Standard form requires `?` then
  // an expression then `:` then a cond_expr.
  if (equal(tok->next, ":"))
    error_tok(tok, "parse_v2: GNU `?:` Elvis form not yet implemented");

  Node *then_n = expr(&tok, tok->next);
  tok = skip(tok, ":");
  Node *els_n = cond_expr(&tok, tok);

  Node *result = new_node(ND_COND, q);
  result->cond = node;
  result->then = then_n;
  result->els = els_n;
  *rest = tok;
  return result;
}

static Node *logor(Token **rest, Token *tok) {
  Node *node = logand(&tok, tok);
  while (equal(tok, "||")) {
    Token *op = tok;
    Node *rhs = logand(&tok, tok->next);
    node = new_binary(ND_LOGOR, node, rhs, op);
  }
  *rest = tok;
  return node;
}

static Node *logand(Token **rest, Token *tok) {
  Node *node = bitor_(&tok, tok);
  while (equal(tok, "&&")) {
    Token *op = tok;
    Node *rhs = bitor_(&tok, tok->next);
    node = new_binary(ND_LOGAND, node, rhs, op);
  }
  *rest = tok;
  return node;
}

static Node *bitor_(Token **rest, Token *tok) {
  Node *node = bitxor(&tok, tok);
  while (equal(tok, "|")) {
    Token *op = tok;
    Node *rhs = bitxor(&tok, tok->next);
    node = new_binary(ND_BITOR, node, rhs, op);
  }
  *rest = tok;
  return node;
}

static Node *bitxor(Token **rest, Token *tok) {
  Node *node = bitand_(&tok, tok);
  while (equal(tok, "^")) {
    Token *op = tok;
    Node *rhs = bitand_(&tok, tok->next);
    node = new_binary(ND_BITXOR, node, rhs, op);
  }
  *rest = tok;
  return node;
}

static Node *bitand_(Token **rest, Token *tok) {
  Node *node = equality(&tok, tok);
  while (equal(tok, "&")) {
    Token *op = tok;
    Node *rhs = equality(&tok, tok->next);
    node = new_binary(ND_BITAND, node, rhs, op);
  }
  *rest = tok;
  return node;
}

static Node *equality(Token **rest, Token *tok) {
  Node *node = relational(&tok, tok);
  for (;;) {
    Token *op = tok;
    if (equal(tok, "==")) {
      Node *rhs = relational(&tok, tok->next);
      node = new_binary(ND_EQ, node, rhs, op);
    } else if (equal(tok, "!=")) {
      Node *rhs = relational(&tok, tok->next);
      node = new_binary(ND_NE, node, rhs, op);
    } else break;
  }
  *rest = tok;
  return node;
}

static Node *relational(Token **rest, Token *tok) {
  Node *node = shift(&tok, tok);
  for (;;) {
    Token *op = tok;
    if (equal(tok, "<")) {
      Node *rhs = shift(&tok, tok->next);
      node = new_binary(ND_LT, node, rhs, op);
    } else if (equal(tok, "<=")) {
      Node *rhs = shift(&tok, tok->next);
      node = new_binary(ND_LE, node, rhs, op);
    } else if (equal(tok, ">")) {
      // `>` lowers to `ND_LT` with operands swapped.
      Node *rhs = shift(&tok, tok->next);
      node = new_binary(ND_LT, rhs, node, op);
    } else if (equal(tok, ">=")) {
      Node *rhs = shift(&tok, tok->next);
      node = new_binary(ND_LE, rhs, node, op);
    } else break;
  }
  *rest = tok;
  return node;
}

static Node *shift(Token **rest, Token *tok) {
  Node *node = add(&tok, tok);
  for (;;) {
    Token *op = tok;
    if (equal(tok, "<<")) {
      Node *rhs = add(&tok, tok->next);
      node = new_binary(ND_SHL, node, rhs, op);
    } else if (equal(tok, ">>")) {
      Node *rhs = add(&tok, tok->next);
      node = new_binary(ND_SHR, node, rhs, op);
    } else break;
  }
  *rest = tok;
  return node;
}

static Node *add(Token **rest, Token *tok) {
  Node *node = mul(&tok, tok);
  for (;;) {
    Token *op = tok;
    if (equal(tok, "+")) {
      Node *rhs = mul(&tok, tok->next);
      node = new_add(node, rhs, op);
    } else if (equal(tok, "-")) {
      Node *rhs = mul(&tok, tok->next);
      node = new_sub(node, rhs, op);
    } else break;
  }
  *rest = tok;
  return node;
}

static Node *mul(Token **rest, Token *tok) {
  Node *node = cast(&tok, tok);
  for (;;) {
    Token *op = tok;
    if (equal(tok, "*")) {
      Node *rhs = cast(&tok, tok->next);
      node = new_binary(ND_MUL, node, rhs, op);
    } else if (equal(tok, "/")) {
      Node *rhs = cast(&tok, tok->next);
      node = new_binary(ND_DIV, node, rhs, op);
    } else if (equal(tok, "%")) {
      Node *rhs = cast(&tok, tok->next);
      node = new_binary(ND_MOD, node, rhs, op);
    } else break;
  }
  *rest = tok;
  return node;
}

// to_assign — lower a compound-assign-style binary into an
// address-stash + indirect-update comma chain (04b §C.5):
//   binary  = lhs op rhs                  (input)
//   result  = (tmp = &lhs, *tmp = *tmp op rhs)
// Pointer arithmetic in `binary` is already scaled (caller used
// new_add / new_sub when applicable), so `new_binary(binary->kind,
// *tmp, rhs)` is correct for both numeric and ptr+int.
static Node *to_assign(Node *binary) {
  add_type(binary->lhs);
  add_type(binary->rhs);
  Token *tok = binary->tok;

  Obj *tmp = new_lvar(new_unique_name(), pointer_to(binary->lhs->ty));

  Node *tmp_ref1 = new_var_node(tmp, tok);
  Node *addr = new_unary(ND_ADDR, binary->lhs, tok);
  Node *expr1 = new_binary(ND_ASSIGN, tmp_ref1, addr, tok);

  Node *tmp_ref2 = new_var_node(tmp, tok);
  Node *deref_l = new_unary(ND_DEREF, tmp_ref2, tok);
  Node *tmp_ref3 = new_var_node(tmp, tok);
  Node *deref_r = new_unary(ND_DEREF, tmp_ref3, tok);
  // binary->rhs is already scaled for pointer arithmetic (caller used
  // new_add/new_sub before to_assign), so don't re-route through
  // new_add/new_sub here — that would double-scale.  Mirrors canonical
  // parse.c §4.5 to_assign (line 2294).
  Node *op_node = new_binary(binary->kind, deref_r, binary->rhs, tok);
  Node *expr2 = new_binary(ND_ASSIGN, deref_l, op_node, tok);

  return new_binary(ND_COMMA, expr1, expr2, tok);
}

// new_inc_dec — post-inc/dec lowering (04b §H.3, non-bitfield):
//   (tmp = &x, old = *tmp, *tmp = *tmp + addend, old)
// addend is +1 / -1 (encoded as int, scaled by new_add for ptr).
static Node *new_inc_dec(Node *node, Token *tok, int addend) {
  add_type(node);
  Obj *tmp = new_lvar(new_unique_name(), pointer_to(node->ty));
  Obj *old = new_lvar(new_unique_name(), node->ty);

  Node *tmp1 = new_var_node(tmp, tok);
  Node *addr = new_unary(ND_ADDR, node, tok);
  Node *e1 = new_binary(ND_ASSIGN, tmp1, addr, tok);

  Node *tmp2 = new_var_node(tmp, tok);
  Node *deref1 = new_unary(ND_DEREF, tmp2, tok);
  Node *old_ref = new_var_node(old, tok);
  Node *e2 = new_binary(ND_ASSIGN, old_ref, deref1, tok);

  Node *tmp3 = new_var_node(tmp, tok);
  Node *deref_l = new_unary(ND_DEREF, tmp3, tok);
  Node *tmp4 = new_var_node(tmp, tok);
  Node *deref_r = new_unary(ND_DEREF, tmp4, tok);
  Node *delta = new_num(addend, tok);
  Node *sum = new_add(deref_r, delta, tok);
  Node *e3 = new_binary(ND_ASSIGN, deref_l, sum, tok);

  Node *result = new_var_node(old, tok);
  // Chain: (e1, (e2, (e3, result)))
  Node *t = new_binary(ND_COMMA, e3, result, tok);
  t = new_binary(ND_COMMA, e2, t, tok);
  t = new_binary(ND_COMMA, e1, t, tok);
  return t;
}

// new_ulong — convenience for sizeof / _Alignof results.
static Node *new_ulong(uint64_t val, Token *tok) {
  Node *node = new_num((int64_t)val, tok);
  node->ty = ty_ulong;
  return node;
}

// typename_ — declspec + abstract declarator (04a §C.8).  Used by
// sizeof / cast / _Alignof / etc.
static Type *typename_(Token **rest, Token *tok) {
  Type *ty = declspec(&tok, tok, NULL);
  ty = declarator(&tok, tok, ty);
  *rest = tok;
  return ty;
}

// const_expr_val — parse a cond_expr and fold (04b §K).
static int64_t const_expr_val(Token **rest, Token *tok) {
  Node *node = cond_expr(&tok, tok);
  add_type(node);
  int64_t v = eval_node(node);
  *rest = tok;
  return v;
}


// cast — 04b §F.  Disambiguate `( typename )`:
//   - followed by `{` → compound literal — defer to unary so
//     postfix's prologue handles it.
//   - otherwise → cast: parse cast operand, wrap in ND_CAST.
//   - `( expr )` → fall through to unary.
static Node *cast(Token **rest, Token *tok) {
  if (equal(tok, "(") && is_typename(tok->next)) {
    // Two-token-lookahead disambiguator: peek past the closing `)`
    // to see if this is a compound literal.  If so, return the
    // original tok (untouched) to unary so postfix can re-parse.
    Token *probe = tok->next;
    Type *throwaway = NULL;
    (void)throwaway;
    Token *after_type;
    typename_(&after_type, probe);
    if (equal(after_type, ")") && equal(after_type->next, "{")) {
      return unary(rest, tok);
    }

    // True cast.
    Type *ty = typename_(&tok, tok->next);
    tok = skip(tok, ")");
    Node *operand = cast(rest, tok);
    Node *node = new_cast(operand, ty);
    add_type(node);
    return node;
  }
  return unary(rest, tok);
}

// unary — 04b §G prefix operators.  Subset:
//   +x, -x, &x, *x, !x, ~x, sizeof
// Pre-inc/dec, __real__/__imag__, &&label, _Alignof deferred.
static Node *unary(Token **rest, Token *tok) {
  // §G.1 — unary plus is a no-op at the parser level.
  if (equal(tok, "+"))
    return cast(rest, tok->next);

  // §G.2 — unary minus.
  if (equal(tok, "-")) {
    Token *op = tok;
    Node *operand = cast(rest, tok->next);
    return new_unary(ND_NEG, operand, op);
  }

  // §G.3 — address-of.
  if (equal(tok, "&")) {
    Token *op = tok;
    Node *operand = cast(rest, tok->next);
    return new_unary(ND_ADDR, operand, op);
  }

  // §G.4 — indirection.
  if (equal(tok, "*")) {
    Token *op = tok;
    Node *operand = cast(rest, tok->next);
    Node *node = new_unary(ND_DEREF, operand, op);
    add_type(node);
    return node;
  }

  // §G.5 — logical not.
  if (equal(tok, "!")) {
    Token *op = tok;
    Node *operand = cast(rest, tok->next);
    return new_unary(ND_NOT, operand, op);
  }

  // §G.9 — &&label (labels-as-values, GCC).  Reuse a prior matching
  // ND_LABEL_VAL's unique_label so multiple references resolve to
  // the same emitted label.
  if (equal(tok, "&&")) {
    if (tok->next->kind != TK_IDENT)
      error_tok(tok->next, "expected label after `&&`");
    Token *lab_tok = tok->next;
    char *name = strndup_checked(lab_tok->loc, lab_tok->len);

    char *uniq = NULL;
    for (Node *g = gotos; g; g = g->goto_next) {
      if (g->kind == ND_LABEL_VAL && g->label && !strcmp(g->label, name)) {
        uniq = g->unique_label;
        break;
      }
    }
    if (!uniq)
      uniq = format("_cg_%d", label_cnt++);

    Node *node = new_node(ND_LABEL_VAL, tok);
    node->label = name;
    node->unique_label = uniq;
    node->goto_next = gotos;
    gotos = node;
    *rest = lab_tok->next;
    return node;
  }

  // §G.6 — bitwise not.
  if (equal(tok, "~")) {
    Token *op = tok;
    Node *operand = cast(rest, tok->next);
    return new_unary(ND_BITNOT, operand, op);
  }

  // §G.7 — __real__ x / __imag__ x.
  if (equal(tok, "__real__") || equal(tok, "__real")) {
    Token *op = tok;
    Node *operand = cast(rest, tok->next);
    Node *node = new_unary(ND_REAL, operand, op);
    add_type(node);
    return node;
  }
  if (equal(tok, "__imag__") || equal(tok, "__imag")) {
    Token *op = tok;
    Node *operand = cast(rest, tok->next);
    Node *node = new_unary(ND_IMAG, operand, op);
    add_type(node);
    return node;
  }

  // §G.8 — pre-inc/dec: ++x  /  --x.
  // For non-bitfield: to_assign(new_add(x, ±1)).  Bitfield path
  // deferred until struct support lands.
  if (equal(tok, "++") || equal(tok, "--")) {
    Token *op = tok;
    int addend = equal(tok, "++") ? 1 : -1;
    Node *operand = unary(rest, tok->next);
    Node *delta = new_num(addend, op);
    Node *sum = new_add(operand, delta, op);
    return to_assign(sum);
  }

  // §G.11 — _Alignof / __alignof__ / __alignof.
  if (equal(tok, "_Alignof") || equal(tok, "__alignof__") || equal(tok, "__alignof")) {
    Token *op = tok;
    if (equal(tok->next, "(") && is_typename(tok->next->next)) {
      Type *ty = typename_(&tok, tok->next->next);
      *rest = skip(tok, ")");
      return new_ulong((uint64_t)ty->align, op);
    }
    Node *operand = unary(rest, tok->next);
    add_type(operand);
    int al = operand->ty->align;
    if (operand->kind == ND_VAR && operand->var && operand->var->align > al)
      al = operand->var->align;
    return new_ulong((uint64_t)al, op);
  }

  // §G.10 — sizeof.  Both forms.
  if (equal(tok, "sizeof")) {
    Token *op = tok;
    // sizeof ( type-name )  — disambiguate by is_typename after `(`.
    if (equal(tok->next, "(") && is_typename(tok->next->next)) {
      Type *ty = typename_(&tok, tok->next->next);
      *rest = skip(tok, ")");
      return new_ulong((uint64_t)ty->size, op);
    }
    Node *operand = unary(rest, tok->next);
    add_type(operand);
    return new_ulong((uint64_t)operand->ty->size, op);
  }

  return postfix(rest, tok);
}

// init_zero_or_struct — 04d-style brace init for compound literal.
// Subset (sufficient for ncc self-host):
//   - {} or {0}                           → memzero only
//   - {expr1, expr2, ...} for struct of   → memzero + per-member ND_ASSIGN
//     scalars; member walks ty->members
//   - {expr1, expr2, ...} for array of    → memzero + indexed *(p+i)=ei
//     scalars; uses new_add for addressing
// Returns a comma chain rooted at ND_MEMZERO; the chain's final
// rhs (just memzero if no inits) carries the var->ty type after
// add_type.
static Node *init_compound_literal(Token **rest, Token *tok, Obj *var) {
  Type *ty = var->ty;
  Token *open = tok;
  tok = skip(tok, "{");
  Node *memzero = new_node(ND_MEMZERO, open);
  memzero->var = var;
  Node *chain = memzero;

  if (ty->kind == TY_ARRAY) {
    long i = 0;
    long cap = ty->array_len;
    bool first = true;
    while (!equal(tok, "}")) {
      if (!first) tok = skip(tok, ",");
      first = false;
      if (equal(tok, "}")) break;
      if (equal(tok, "{"))
        error_tok(tok, "parse_v2: nested brace init in compound literal not supported");
      Node *e = assign(&tok, tok);
      add_type(e);
      Node *vref = new_var_node(var, open);
      Node *sum = new_add(vref, new_num(i, open), open);
      Node *deref = new_unary(ND_DEREF, sum, open);
      Node *as = new_binary(ND_ASSIGN, deref, e, open);
      chain = new_binary(ND_COMMA, chain, as, open);
      i++;
      if (cap >= 0 && i >= cap) {
        while (!equal(tok, "}") && !equal(tok, ",")) tok = tok->next;
      }
    }
    tok = skip(tok, "}");
    if (cap < 0) {
      var->ty = array_of(ty->base, i);
    }
  } else if (ty->kind == TY_STRUCT) {
    Member *m = ty->members;
    bool first = true;
    while (!equal(tok, "}")) {
      if (!first) tok = skip(tok, ",");
      first = false;
      if (equal(tok, "}")) break;
      if (!m)
        error_tok(tok, "excess elements in struct compound literal");
      if (equal(tok, "{"))
        error_tok(tok, "parse_v2: nested brace init in compound literal not supported");
      Node *e = assign(&tok, tok);
      Node *vref = new_var_node(var, open);
      Node *mn = new_unary(ND_MEMBER, vref, open);
      mn->member = m;
      Node *as = new_binary(ND_ASSIGN, mn, e, open);
      chain = new_binary(ND_COMMA, chain, as, open);
      m = m->next;
    }
    tok = skip(tok, "}");
  } else {
    // Scalar T x = {expr};
    if (!equal(tok, "}")) {
      Node *e = assign(&tok, tok);
      Node *vref = new_var_node(var, open);
      Node *as = new_binary(ND_ASSIGN, vref, e, open);
      chain = new_binary(ND_COMMA, chain, as, open);
      if (equal(tok, ",")) tok = tok->next;
    }
    tok = skip(tok, "}");
  }
  *rest = tok;
  return chain;
}

// postfix — 04b §H suffix loop.  Subset:
//   primary [ expr ]
//   primary ( arg-list )
//   primary . ident       (deferred until struct lands)
//   primary -> ident      (deferred)
//   primary ++ / --       (deferred — needs to_assign)
static Node *postfix(Token **rest, Token *tok) {
  Node *node = NULL;

  // Compound literal at head: ( typename ) { ... } — 04b §H.1.
  if (equal(tok, "(") && is_typename(tok->next)) {
    Token *probe;
    Type *cl_ty = typename_(&probe, tok->next);
    if (equal(probe, ")") && equal(probe->next, "{")) {
      tok = probe->next;
      if (current_fn) {
        Obj *anon = new_lvar(new_unique_name(), cl_ty);
        Node *chain = init_compound_literal(&tok, tok, anon);
        Node *vref = new_var_node(anon, tok);
        node = new_binary(ND_COMMA, chain, vref, tok);
      } else {
        // File-scope: anon gvar holding the laid-out struct/array
        // bytes.  Subset: struct of integer/pointer scalars with no
        // bit-fields, and arrays of integer scalars.  Field values
        // fold via const_expr_val.
        Obj *anon = new_anon_gvar(cl_ty);
        anon->is_static = true;
        long sz = cl_ty->size;
        char *buf = calloc_checked(1, sz);
        Token *open = tok;
        tok = skip(tok, "{");
        if (cl_ty->kind == TY_STRUCT) {
          Member *m = cl_ty->members;
          bool first = true;
          while (!equal(tok, "}")) {
            if (!first) tok = skip(tok, ",");
            first = false;
            if (equal(tok, "}")) break;
            // Designator: `.name = expr` repositions to the named member.
            if (equal(tok, ".")) {
              tok = tok->next;
              if (tok->kind != TK_IDENT)
                error_tok(tok, "expected member name after `.`");
              Member *target = find_member(cl_ty, tok);
              if (!target)
                error_tok(tok, "no such member");
              tok = skip(tok->next, "=");
              m = target;
            }
            if (!m) error_tok(tok, "excess elements in struct compound literal");
            int64_t v = const_expr_val(&tok, tok);
            for (int b = 0; b < (int)m->ty->size; b++)
              buf[m->offset + b] = (v >> (b * 8)) & 0xff;
            m = m->next;
          }
          tok = skip(tok, "}");
        } else if (cl_ty->kind == TY_ARRAY && is_integer(cl_ty->base)) {
          long elem_sz = cl_ty->base->size;
          long i = 0;
          bool first = true;
          while (!equal(tok, "}")) {
            if (!first) tok = skip(tok, ",");
            first = false;
            if (equal(tok, "}")) break;
            int64_t v = const_expr_val(&tok, tok);
            if ((i + 1) * elem_sz <= sz) {
              for (int b = 0; b < (int)elem_sz; b++)
                buf[i * elem_sz + b] = (v >> (b * 8)) & 0xff;
            }
            i++;
          }
          tok = skip(tok, "}");
        } else {
          error_tok(open, "parse_v2: this file-scope compound literal type not supported");
        }
        anon->init_data = buf;
        anon->init_data_size = sz;
        node = new_var_node(anon, open);
      }
    }
  }

  if (!node)
    node = primary(&tok, tok);

  for (;;) {
    // [ expr ]  — *(node + idx).
    if (equal(tok, "[")) {
      Token *op = tok;
      Node *idx = expr(&tok, tok->next);
      tok = skip(tok, "]");
      Node *sum = new_add(node, idx, op);
      node = new_unary(ND_DEREF, sum, op);
      add_type(node);
      continue;
    }

    // . ident — direct member access.
    if (equal(tok, ".")) {
      if (tok->next->kind != TK_IDENT)
        error_tok(tok->next, "expected identifier after `.`");
      node = struct_ref(node, tok->next);
      tok = tok->next->next;
      continue;
    }

    // -> ident — pointer member access.  Implicit deref then `.`.
    if (equal(tok, "->")) {
      if (tok->next->kind != TK_IDENT)
        error_tok(tok->next, "expected identifier after `->`");
      Token *arrow = tok;
      Node *deref = new_unary(ND_DEREF, node, arrow);
      node = struct_ref(deref, tok->next);
      tok = tok->next->next;
      continue;
    }

    // ++ / -- (post-inc/dec).  04b §H.3 lowering — comma chain
    // returning the original value.
    if (equal(tok, "++") || equal(tok, "--")) {
      Token *op = tok;
      int addend = equal(tok, "++") ? 1 : -1;
      node = new_inc_dec(node, op, addend);
      tok = tok->next;
      continue;
    }

    // ( arg-list )  — function call.
    if (equal(tok, "(")) {
      add_type(node);
      Type *fn_ty = NULL;
      if (node->ty->kind == TY_FUNC)
        fn_ty = node->ty;
      else if (node->ty->kind == TY_PTR && node->ty->base->kind == TY_FUNC)
        fn_ty = node->ty->base;
      if (!fn_ty) break;  // not callable; let caller fail.

      Token *call_tok = tok;
      tok = tok->next;

      Node head = {0};
      Node *cur = &head;
      Type *param_ty = fn_ty->params;
      while (!equal(tok, ")")) {
        if (cur != &head) tok = skip(tok, ",");
        Node *arg = assign(&tok, tok);
        add_type(arg);
        if (param_ty) {
          if (param_ty->kind != TY_STRUCT && param_ty->kind != TY_UNION &&
              param_ty->kind != TY_VECTOR)
            arg = new_cast(arg, param_ty);
          param_ty = param_ty->next;
        } else if (arg->ty->kind == TY_FLOAT) {
          // Default argument promotion (variadic): float → double.
          arg = new_cast(arg, ty_double);
        }
        cur = cur->next = arg;
      }

      Node *call = new_node(ND_FUNCALL, call_tok);
      call->lhs = node;
      call->func_ty = fn_ty;
      call->ty = fn_ty->return_ty;
      call->args = head.next;
      if (node->kind == ND_VAR)
        call->funcname = node->var->name;
      else
        call->funcname = "__indirect_call";

      tok = skip(tok, ")");
      node = call;
      continue;
    }

    break;
  }

  *rest = tok;
  return node;
}

// new_var_node — wrap an Obj into an ND_VAR node.
static Node *new_var_node(Obj *var, Token *tok) {
  Node *node = new_node(ND_VAR, tok);
  node->var = var;
  return node;
}

// new_string_literal — anon global containing the string bytes.
// 04b §H.4 step 5: adjacent literals are already concatenated by
// the tokenizer (per the convert pass), so a single TK_STR fully
// describes the string.
static Obj *new_anon_gvar(Type *ty) {
  Obj *var = new_gvar(new_unique_name(), ty);
  var->is_static = true;
  var->is_definition = true;
  return var;
}

// primary — 04b §H.4 dispatch.  Vertical-slice subset:
//   3. ( expr )
//   5. TK_STR
//   6. TK_NUM
//   8. IDENT (variable / enum constant; calls handled later via postfix)
static Node *primary(Token **rest, Token *tok) {
  // GNU statement expression: ( { stmt-list } ).  The value is the
  // last expression-statement's value.  04c §G + 04b §H.4 step 2.
  if (equal(tok, "(") && equal(tok->next, "{")) {
    Token *open = tok;
    Node *body = compound_stmt(&tok, tok->next->next);
    Node *node = new_node(ND_STMT_EXPR, open);
    node->body = body->body;
    *rest = skip(tok, ")");
    return node;
  }

  // ( expr )
  if (equal(tok, "(")) {
    Node *node = expr(&tok, tok->next);
    *rest = skip(tok, ")");
    return node;
  }

  // String literal.  Adjacent string literals concatenate per C11
  // 5.1.1.2 phase 6 (e.g. `"foo" "bar"` -> `"foobar"`); since the
  // tokenizer keeps them as separate tokens, do the merge here.
  // Mirrors canonical parse.c:3790.
  if (tok->kind == TK_STR) {
    if (tok->next->kind == TK_STR) {
      int total_len = 0;
      for (Token *t = tok; t->kind == TK_STR; t = t->next)
        total_len += t->ty->array_len - 1;  // -1 to drop NUL
      total_len++;  // final NUL
      char *buf = calloc_checked(1, total_len);
      int offset = 0;
      Token *first = tok;
      for (; tok->kind == TK_STR; tok = tok->next) {
        int len = tok->ty->array_len - 1;
        memcpy(buf + offset, tok->str, len);
        offset += len;
      }
      buf[offset] = '\0';
      Type *cat_ty = array_of(ty_char, total_len);
      Obj *var = new_anon_gvar(cat_ty);
      var->init_data = buf;
      var->init_data_size = cat_ty->size;
      Node *node = new_var_node(var, first);
      node->ty = cat_ty;
      *rest = tok;
      return node;
    }
    Obj *var = new_anon_gvar(tok->ty);
    var->init_data = tok->str;
    var->init_data_size = tok->ty->size;
    Node *node = new_var_node(var, tok);
    node->ty = tok->ty;
    *rest = tok->next;
    return node;
  }

  // Numeric literal.
  if (tok->kind == TK_NUM) {
    Node *node;
    if (tok->ty && is_flonum(tok->ty)) {
      node = new_node(ND_NUM, tok);
      node->fval = tok->fval;
      node->ty = tok->ty;
    } else {
      node = new_num(tok->val, tok);
      if (tok->ty)
        node->ty = tok->ty;
    }
    *rest = tok->next;
    return node;
  }

  // _Generic ( expr, T1: a1, T2: a2, ..., default: ad )
  // 04b §H.4 step 4.  Match by type-kind + size; the controlling
  // expression's value is not evaluated — only its type is used.
  if (equal(tok, "_Generic")) {
    Token *gen = tok;
    tok = skip(tok->next, "(");
    Node *ctrl = assign(&tok, tok);
    add_type(ctrl);
    Type *cty = ctrl->ty;
    Node *match = NULL;
    Node *default_n = NULL;
    while (equal(tok, ",")) {
      tok = tok->next;
      if (equal(tok, "default")) {
        tok = skip(tok->next, ":");
        Node *e = assign(&tok, tok);
        default_n = e;
      } else {
        Type *t = typename_(&tok, tok);
        tok = skip(tok, ":");
        Node *e = assign(&tok, tok);
        if (!match && cty &&
            t->kind == cty->kind && t->size == cty->size &&
            t->is_unsigned == cty->is_unsigned)
          match = e;
      }
    }
    tok = skip(tok, ")");
    if (!match) match = default_n;
    if (!match)
      error_tok(gen, "no matching _Generic association");
    *rest = tok;
    return match;
  }

  // __func__ / __FUNCTION__ / __PRETTY_FUNCTION__ — predefined
  // identifier expanding to a string literal containing the
  // current function's name (04b §H.4 step 7).
  if (tok->kind == TK_IDENT &&
      (tok_name_eq(tok, "__func__") ||
       tok_name_eq(tok, "__FUNCTION__") ||
       tok_name_eq(tok, "__PRETTY_FUNCTION__"))) {
    const char *fn_name = current_fn ? current_fn->name : "";
    size_t n = strlen(fn_name);
    Type *aty = array_of(ty_char, (long)n + 1);
    Obj *anon = new_anon_gvar(aty);
    char *buf = calloc_checked(1, n + 1);
    memcpy(buf, fn_name, n);
    anon->init_data = buf;
    anon->init_data_size = (int)(n + 1);
    Node *node = new_var_node(anon, tok);
    node->ty = aty;
    *rest = tok->next;
    return node;
  }

  // Identifier — first dispatch known builtins, then fall through
  // to var/enum/implicit-call lookup.
  if (tok->kind == TK_IDENT) {
    // __builtin_expect(expr, c)  — returns expr.
    if (tok_name_eq(tok, "__builtin_expect")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      Node *first = assign(&tok, tok);
      tok = skip(tok, ",");
      // Discard the hint expression value.
      assign(&tok, tok);
      tok = skip(tok, ")");
      *rest = tok;
      add_type(first);
      (void)bi;
      return first;
    }

    // __builtin_constant_p(expr) — folds to 1 if try_eval_node
    // succeeds on the operand, else 0.  04b §M.2.
    if (tok_name_eq(tok, "__builtin_constant_p")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      Node *e = assign(&tok, tok);
      add_type(e);
      tok = skip(tok, ")");
      int64_t v;
      Node *node = new_num(try_eval_node(e, &v) ? 1 : 0, bi);
      *rest = tok;
      return node;
    }

    // __builtin_types_compatible_p(t1, t2) — folds to 1 iff the two
    // types are compatible.  Approximation: same kind + same size.
    if (tok_name_eq(tok, "__builtin_types_compatible_p")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      Type *t1 = typename_(&tok, tok);
      tok = skip(tok, ",");
      Type *t2 = typename_(&tok, tok);
      tok = skip(tok, ")");
      int compat = (t1->kind == t2->kind && t1->size == t2->size);
      Node *node = new_num(compat, bi);
      *rest = tok;
      return node;
    }

    // __builtin_unreachable() — emit a return of 0 (parser-only
    // approximation; codegen has no unreachable hint of its own).
    if (tok_name_eq(tok, "__builtin_unreachable")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      tok = skip(tok, ")");
      *rest = tok;
      return new_num(0, bi);
    }

    // __builtin_bswap16(x) — table can't represent its
    // result-type/val convention (uses BSWAP32 with val=16 to flag
    // 16-bit truncation in codegen).  Mirrors canonical parse.c:3437.
    if (tok_name_eq(tok, "__builtin_bswap16")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      Node *arg = assign(&tok, tok);
      tok = skip(tok, ")");
      *rest = tok;
      Node *node = new_unary(ND_BUILTIN_BSWAP32, arg, bi);
      node->ty = ty_ushort;
      node->val = 16;
      return node;
    }

    // Bit-manipulation builtins (table at file scope).  Each is a
    // single-argument node lowered via ND_BUILTIN_*; node->val is
    // a 32-vs-64 boolean codegen reads.
    for (size_t i = 0; i < sizeof(unary_builtin_table)/sizeof(*unary_builtin_table); i++) {
      if (!tok_name_eq(tok, unary_builtin_table[i].name)) continue;
      Token *bi = tok;
      tok = skip(tok->next, "(");
      Node *arg = assign(&tok, tok);
      tok = skip(tok, ")");
      *rest = tok;
      Node *node = new_unary(unary_builtin_table[i].kind, arg, bi);
      node->ty = ty_int;
      node->val = unary_builtin_table[i].is64 ? 1 : 0;
      return node;
    }

    // __builtin_offsetof(type, member) — folded.  Subset: top-level
    // member only (no `.sub.sub` or `[i]` chain).  Wider chains land
    // when needed.
    if (tok_name_eq(tok, "__builtin_offsetof")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      Type *ty = typename_(&tok, tok);
      tok = skip(tok, ",");
      if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
        error_tok(bi, "offsetof requires struct/union type");
      if (tok->kind != TK_IDENT)
        error_tok(tok, "expected member name");
      Member *m = find_member(ty, tok);
      if (!m) {
        // Search anonymous members.
        for (Member *am = ty->members; am; am = am->next) {
          if (am->name) continue;
          if (am->ty->kind != TY_STRUCT && am->ty->kind != TY_UNION) continue;
          Member *inner = find_member(am->ty, tok);
          if (inner) {
            // Return cumulative offset.  Anonymous members aren't
            // bitfields in our subset, so direct add.
            tok = tok->next;
            tok = skip(tok, ")");
            *rest = tok;
            Node *n = new_num(am->offset + inner->offset, bi);
            n->ty = ty_ulong;
            return n;
          }
        }
        error_tok(tok, "no such member");
      }
      tok = tok->next;
      tok = skip(tok, ")");
      *rest = tok;
      Node *n = new_num(m->offset, bi);
      n->ty = ty_ulong;
      return n;
    }

    // __builtin_alloca(size) — emit ND_BUILTIN_ALLOCA with one arg.
    if (tok_name_eq(tok, "__builtin_alloca") || tok_name_eq(tok, "alloca")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      Node *sz = assign(&tok, tok);
      tok = skip(tok, ")");
      *rest = tok;
      Node *n = new_unary(ND_BUILTIN_ALLOCA, sz, bi);
      n->ty = pointer_to(ty_void);
      return n;
    }

    // __builtin_frame_address(N) — ND_BUILTIN_FRAME_ADDR with depth.
    if (tok_name_eq(tok, "__builtin_frame_address")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      int64_t depth = const_expr_val(&tok, tok);
      tok = skip(tok, ")");
      *rest = tok;
      Node *n = new_node(ND_BUILTIN_FRAME_ADDR, bi);
      n->val = depth;
      n->ty = pointer_to(ty_void);
      return n;
    }

    // __builtin_return_address(N) — ND_RETURN_ADDR with depth.
    if (tok_name_eq(tok, "__builtin_return_address")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      int64_t depth = const_expr_val(&tok, tok);
      tok = skip(tok, ")");
      *rest = tok;
      Node *n = new_node(ND_RETURN_ADDR, bi);
      n->val = depth;
      n->ty = pointer_to(ty_void);
      return n;
    }

    // __builtin_(add|sub|mul)_overflow(a, b, &result) — 3-arg form.
    {
      const char *names[] = {"__builtin_add_overflow",
                             "__builtin_sub_overflow",
                             "__builtin_mul_overflow"};
      NodeKind kinds[] = {ND_BUILTIN_ADD_OVERFLOW,
                          ND_BUILTIN_SUB_OVERFLOW,
                          ND_BUILTIN_MUL_OVERFLOW};
      for (int i = 0; i < 3; i++) {
        if (!tok_name_eq(tok, names[i])) continue;
        Token *bi = tok;
        tok = skip(tok->next, "(");
        Node *a = assign(&tok, tok);
        tok = skip(tok, ",");
        Node *b = assign(&tok, tok);
        tok = skip(tok, ",");
        Node *out = assign(&tok, tok);
        tok = skip(tok, ")");
        *rest = tok;
        add_type(out);
        Node *n = new_node(kinds[i], bi);
        n->lhs = a;
        n->rhs = b;
        n->args = out;  // codegen reads result-pointer expression here
        n->overflow_ty = out->ty && out->ty->base ? out->ty->base : ty_int;
        n->ty = ty_int;  // returns 0/1 overflow flag
        return n;
      }
    }

    // __builtin_va_start(ap, last) — Apple ARM64: ap = __va_area__.
    if (tok_name_eq(tok, "__builtin_va_start")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      Node *ap = assign(&tok, tok);
      tok = skip(tok, ",");
      assign(&tok, tok);  // last_named — discarded on ARM64
      tok = skip(tok, ")");
      *rest = tok;
      if (!current_fn || !current_fn->va_area)
        return new_num(0, bi);
      return new_binary(ND_ASSIGN, ap,
               new_cast(new_var_node(current_fn->va_area, bi),
                        pointer_to(ty_void)), bi);
    }

    // __builtin_va_end(ap) — no-op.
    if (tok_name_eq(tok, "__builtin_va_end")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      assign(&tok, tok);  // ap — evaluated and discarded
      tok = skip(tok, ")");
      *rest = tok;
      return new_num(0, bi);
    }

    // __builtin_va_copy(d, s) — d = s.
    if (tok_name_eq(tok, "__builtin_va_copy")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      Node *d = assign(&tok, tok);
      tok = skip(tok, ",");
      Node *s = assign(&tok, tok);
      tok = skip(tok, ")");
      *rest = tok;
      return new_binary(ND_ASSIGN, d, s, bi);
    }

    // __builtin_va_arg(ap, T) — comma chain per 04b §M:
    //   (tmp = (T*)ap, ap = (void*)((char*)ap + align(sizeof(T), 8)), *tmp)
    if (tok_name_eq(tok, "__builtin_va_arg")) {
      Token *bi = tok;
      tok = skip(tok->next, "(");
      Node *ap = assign(&tok, tok);
      tok = skip(tok, ",");
      Type *ty = typename_(&tok, tok);
      tok = skip(tok, ")");
      *rest = tok;
      add_type(ap);

      long sz = ty->size < 8 ? 8 : ty->size;
      sz = (sz + 7) & ~7L;

      Obj *tmp = new_lvar(new_unique_name(), pointer_to(ty));
      Node *save = new_binary(ND_ASSIGN, new_var_node(tmp, bi),
                              new_cast(ap, pointer_to(ty)), bi);
      Node *ap_as_char = new_cast(ap, pointer_to(ty_char));
      Node *bumped = new_binary(ND_ADD, ap_as_char, new_num(sz, bi), bi);
      Node *advance = new_binary(ND_ASSIGN, ap,
                       new_cast(bumped, pointer_to(ty_void)), bi);
      Node *result = new_unary(ND_DEREF, new_var_node(tmp, bi), bi);
      return new_binary(ND_COMMA, save,
               new_binary(ND_COMMA, advance, result, bi), bi);
    }

    VarScope *vs = find_var(tok);
    if (!vs || (!vs->var && !vs->enum_ty)) {
      // Implicit-function declaration (04b §H.5): if next is `(`,
      // synthesize an int-returning function.  Only mark variadic for
      // the known printf-family names — Apple ARM64's variadic ABI
      // forces stack passing, which breaks regular ABI calls to
      // memcpy/memset/etc. otherwise (mirrors canonical parse.c §5.10
      // implicit-decl branch).
      if (equal(tok->next, "(")) {
        char *name = strndup_checked(tok->loc, tok->len);
        bool known_variadic = !strcmp(name, "printf") ||
                              !strcmp(name, "fprintf") ||
                              !strcmp(name, "sprintf") ||
                              !strcmp(name, "snprintf") ||
                              !strcmp(name, "scanf") ||
                              !strcmp(name, "sscanf");
        Type *fn_ty = func_type(ty_int);
        if (known_variadic) {
          fn_ty->is_variadic = true;
          // Synthesize one named (char *) param so the variadic
          // ABI counts the format-string slot correctly.
          bool two_named = !strcmp(name, "fprintf") ||
                           !strcmp(name, "sprintf") ||
                           !strcmp(name, "sscanf");
          bool three_named = !strcmp(name, "snprintf");
          Type *p1 = copy_type(pointer_to(ty_char));
          p1->next = NULL;
          fn_ty->params = p1;
          if (two_named) {
            Type *p2 = copy_type(pointer_to(ty_char));
            p2->next = NULL;
            p1->next = p2;
          } else if (three_named) {
            Type *p2 = copy_type(pointer_to(ty_char));
            Type *p3 = copy_type(pointer_to(ty_char));
            p2->next = p3;
            p3->next = NULL;
            p1->next = p2;
          }
        }
        Obj *fn = new_gvar(name, fn_ty);
        fn->is_function = true;
        fn->is_definition = false;
        Node *node = new_var_node(fn, tok);
        *rest = tok->next;
        return node;
      }
      error_tok(tok, "undefined variable");
    }
    Node *node;
    if (vs->var)
      node = new_var_node(vs->var, tok);
    else
      node = new_num(vs->enum_val, tok);
    *rest = tok->next;
    return node;
  }

  error_tok(tok, "parse_v2: expected an expression");
}

//
// Public surface stubs (expr-zone — fills come in 04b chunks).
//

Node *new_cast(Node *expr, Type *ty) {
  add_type(expr);
  Node *node = calloc_checked(1, sizeof(Node));
  node->kind = ND_CAST;
  node->tok = expr->tok;
  node->lhs = expr;
  node->ty = copy_type(ty);
  return node;
}

// try_eval_node — non-fatal integer constant folder per 04b §K.1.
// Handles the integer-only operator set; returns false on anything
// not in the switch.  Float / pointer-relocation / builtin folds
// are eval2's job (deferred until the public eval_node grows).
bool try_eval_node(Node *node, int64_t *out) {
  if (!node) return false;
  int64_t lv, rv;
  switch (node->kind) {
  case ND_NUM:    *out = node->val; return true;
  case ND_NEG:
    if (!try_eval_node(node->lhs, &lv)) return false;
    *out = -lv; return true;
  case ND_NOT:
    if (!try_eval_node(node->lhs, &lv)) return false;
    *out = !lv; return true;
  case ND_BITNOT:
    if (!try_eval_node(node->lhs, &lv)) return false;
    *out = ~lv; return true;
  case ND_CAST: {
    // If the source is float-typed, fold the float and truncate to int.
    if (node->lhs && node->lhs->ty && is_flonum(node->lhs->ty) &&
        is_integer(node->ty)) {
      double dv;
      if (!try_eval_double_v2(node->lhs, &dv)) return false;
      int64_t iv = (int64_t)dv;
      switch (node->ty->size) {
      case 1: *out = node->ty->is_unsigned ? (uint8_t)iv  : (int8_t)iv;  return true;
      case 2: *out = node->ty->is_unsigned ? (uint16_t)iv : (int16_t)iv; return true;
      case 4: *out = node->ty->is_unsigned ? (uint32_t)iv : (int32_t)iv; return true;
      default: *out = iv; return true;
      }
    }
    if (!try_eval_node(node->lhs, &lv)) return false;
    if (!node->ty) { *out = lv; return true; }
    if (is_integer(node->ty)) {
      switch (node->ty->size) {
      case 1: *out = node->ty->is_unsigned ? (uint8_t)lv  : (int8_t)lv;  return true;
      case 2: *out = node->ty->is_unsigned ? (uint16_t)lv : (int16_t)lv; return true;
      case 4: *out = node->ty->is_unsigned ? (uint32_t)lv : (int32_t)lv; return true;
      default: *out = lv; return true;
      }
    }
    // Pointer cast: pass-through (e.g. (void*)0 → 0).  Float-typed
    // casts still bail.
    if (node->ty->kind == TY_PTR) {
      *out = lv;
      return true;
    }
    return false;
  }
  case ND_ADD:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    *out = lv + rv; return true;
  case ND_SUB:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    *out = lv - rv; return true;
  case ND_MUL:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    *out = lv * rv; return true;
  case ND_DIV:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    if (rv == 0) return false;
    *out = lv / rv; return true;
  case ND_MOD:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    if (rv == 0) return false;
    *out = lv % rv; return true;
  case ND_BITAND:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    *out = lv & rv; return true;
  case ND_BITOR:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    *out = lv | rv; return true;
  case ND_BITXOR:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    *out = lv ^ rv; return true;
  case ND_SHL:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    *out = lv << rv; return true;
  case ND_SHR:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    if (node->lhs->ty && node->lhs->ty->is_unsigned)
      *out = (int64_t)((uint64_t)lv >> rv);
    else
      *out = lv >> rv;
    return true;
  case ND_EQ:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    *out = (lv == rv); return true;
  case ND_NE:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    *out = (lv != rv); return true;
  case ND_LT:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    if (node->lhs->ty && node->lhs->ty->is_unsigned)
      *out = ((uint64_t)lv < (uint64_t)rv);
    else
      *out = (lv < rv);
    return true;
  case ND_LE:
    if (!try_eval_node(node->lhs, &lv) || !try_eval_node(node->rhs, &rv)) return false;
    if (node->lhs->ty && node->lhs->ty->is_unsigned)
      *out = ((uint64_t)lv <= (uint64_t)rv);
    else
      *out = (lv <= rv);
    return true;
  case ND_LOGAND:
    if (!try_eval_node(node->lhs, &lv)) return false;
    if (!lv) { *out = 0; return true; }
    if (!try_eval_node(node->rhs, &rv)) return false;
    *out = (rv != 0); return true;
  case ND_LOGOR:
    if (!try_eval_node(node->lhs, &lv)) return false;
    if (lv) { *out = 1; return true; }
    if (!try_eval_node(node->rhs, &rv)) return false;
    *out = (rv != 0); return true;
  case ND_COND: {
    int64_t c;
    if (!try_eval_node(node->cond, &c)) return false;
    return try_eval_node(c ? node->then : node->els, out);
  }
  default:
    return false;
  }
}

int64_t eval_node(Node *node) {
  int64_t v;
  if (!try_eval_node(node, &v))
    error_tok(node ? node->tok : NULL, "not a compile-time constant");
  return v;
}

// try_eval_double_v2 — non-fatal floating-point constant folder.
// Used by parse_gvar_initializer's float arms.  Apple ARM64 long double
// is 64-bit, so a `double` accumulator preserves the full range.  Mixed
// int/float fold uses try_eval_node for the integer side.  Bails on
// anything outside the supported set (caller surfaces the diagnostic).
static bool try_eval_double_v2(Node *n, double *out) {
  if (!n) return false;
  double lv, rv;
  int64_t iv;
  switch (n->kind) {
  case ND_NUM:
    if (n->ty && is_flonum(n->ty)) { *out = (double)n->fval; return true; }
    if (n->ty && n->ty->is_unsigned) { *out = (double)(uint64_t)n->val; return true; }
    *out = (double)n->val; return true;
  case ND_NEG:
    if (!try_eval_double_v2(n->lhs, &lv)) return false;
    *out = -lv; return true;
  case ND_ADD:
    if (!try_eval_double_v2(n->lhs, &lv)) return false;
    if (!try_eval_double_v2(n->rhs, &rv)) return false;
    *out = lv + rv; return true;
  case ND_SUB:
    if (!try_eval_double_v2(n->lhs, &lv)) return false;
    if (!try_eval_double_v2(n->rhs, &rv)) return false;
    *out = lv - rv; return true;
  case ND_MUL:
    if (!try_eval_double_v2(n->lhs, &lv)) return false;
    if (!try_eval_double_v2(n->rhs, &rv)) return false;
    *out = lv * rv; return true;
  case ND_DIV:
    if (!try_eval_double_v2(n->lhs, &lv)) return false;
    if (!try_eval_double_v2(n->rhs, &rv)) return false;
    *out = lv / rv; return true;
  case ND_COND: {
    int64_t c;
    if (!try_eval_node(n->cond, &c)) return false;
    return try_eval_double_v2(c ? n->then : n->els, out);
  }
  case ND_COMMA:
    return try_eval_double_v2(n->rhs, out);
  case ND_CAST:
    if (n->lhs && n->lhs->ty && is_flonum(n->lhs->ty)) {
      if (!try_eval_double_v2(n->lhs, &lv)) return false;
      *out = lv;
      return true;
    }
    if (try_eval_node(n->lhs, &iv)) {
      if (n->lhs->ty && n->lhs->ty->is_unsigned)
        *out = (double)(uint64_t)iv;
      else
        *out = (double)iv;
      return true;
    }
    return false;
  default:
    if (try_eval_node(n, &iv)) {
      if (n->ty && n->ty->is_unsigned)
        *out = (double)(uint64_t)iv;
      else
        *out = (double)iv;
      return true;
    }
    return false;
  }
}

// gvar_subinit_recursive — write the initializer for a value of `ty`
// into `buf` at `base_off`, threading relocations through `*rh_tail`.
// Recurses through nested aggregates.  Returns the token after the
// consumed initializer.
//
// Subset:
//   - Empty `{}` → zero-init (already in buf).
//   - char[] = "literal" or {char-array brace}.
//   - char* = "literal" → anon gvar + relocation.
//   - TY_STRUCT brace with optional .field designators.
//   - TY_UNION brace with first-member or .field.
//   - TY_ARRAY brace with element-wise recursion.
//   - Scalar leaves: integer / pointer (with `&gvar` reloc) / flonum.
static Token *gvar_subinit_recursive(Token *tok, Type *ty, char *buf,
                                      long base_off, long buf_sz,
                                      Relocation **rh_tail) {
  if (equal(tok, "{") && equal(tok->next, "}")) {
    return tok->next->next;
  }

  concat_adjacent_strings(tok);
  if (tok->kind == TK_STR && ty->kind == TY_ARRAY &&
      ty->base->kind == TY_CHAR) {
    long s = tok->ty->array_len;
    long n = ty->array_len > 0 && ty->array_len < s ? ty->array_len : s;
    for (long i = 0; i < n && (base_off + i) < buf_sz; i++)
      buf[base_off + i] = tok->str[i];
    return tok->next;
  }

  if (tok->kind == TK_STR && ty->kind == TY_PTR &&
      ty->base->kind == TY_CHAR) {
    Obj *anon = new_anon_gvar(tok->ty);
    anon->init_data = tok->str;
    anon->init_data_size = tok->ty->size;
    Relocation *r = calloc_checked(1, sizeof(Relocation));
    r->offset = (int)base_off;
    r->label = &anon->name;
    *rh_tail = (*rh_tail)->next = r;
    return tok->next;
  }

  if (equal(tok, "{")) {
    tok = tok->next;
    if (ty->kind == TY_STRUCT) {
      Member *m = ty->members;
      bool first = true;
      while (!equal(tok, "}")) {
        if (!first) tok = skip(tok, ",");
        first = false;
        if (equal(tok, "}")) break;
        if (equal(tok, ".")) {
          tok = tok->next;
          if (tok->kind != TK_IDENT)
            error_tok(tok, "expected member name");
          Member *target = find_member(ty, tok);
          if (!target) error_tok(tok, "no such member");
          tok = skip(tok->next, "=");
          m = target;
        }
        if (!m)
          error_tok(tok, "excess elements in struct initializer");
        tok = gvar_subinit_recursive(tok, m->ty, buf,
                                      base_off + m->offset, buf_sz, rh_tail);
        m = m->next;
      }
      return skip(tok, "}");
    }
    if (ty->kind == TY_UNION) {
      Member *m = ty->members;
      if (equal(tok, ".")) {
        tok = tok->next;
        if (tok->kind != TK_IDENT)
          error_tok(tok, "expected member name");
        Member *target = find_member(ty, tok);
        if (!target) error_tok(tok, "no such member");
        tok = skip(tok->next, "=");
        m = target;
      }
      if (m && !equal(tok, "}"))
        tok = gvar_subinit_recursive(tok, m->ty, buf, base_off, buf_sz, rh_tail);
      if (equal(tok, ",")) tok = tok->next;
      return skip(tok, "}");
    }
    if (ty->kind == TY_ARRAY) {
      long elem_sz = ty->base->size;
      long count = 0;
      bool first = true;
      while (!equal(tok, "}")) {
        if (!first) tok = skip(tok, ",");
        first = false;
        if (equal(tok, "}")) break;
        if (ty->array_len > 0 && count >= ty->array_len)
          break;
        tok = gvar_subinit_recursive(tok, ty->base, buf,
                                      base_off + count * elem_sz, buf_sz,
                                      rh_tail);
        count++;
      }
      return skip(tok, "}");
    }
    error_tok(tok, "parse_v2: brace init for non-aggregate type");
  }

  if (is_flonum(ty)) {
    Node *e = assign(&tok, tok);
    add_type(e);
    double dv;
    if (!try_eval_double_v2(e, &dv))
      error_tok(tok, "parse_v2: gvar scalar float not const-foldable");
    if (ty->kind == TY_FLOAT) {
      float f = (float)dv;
      memcpy(buf + base_off, &f, 4);
    } else {
      long sz = ty->size;
      memcpy(buf + base_off, &dv, sz < 8 ? (size_t)sz : 8);
    }
    return tok;
  }

  if (ty->kind == TY_PTR) {
    Token *probe = tok;
    Node *e = assign(&probe, tok);
    add_type(e);
    char *lbl = NULL;
    int64_t addend = 0;
    if (try_eval_addr_v2(e, &lbl, &addend)) {
      Relocation *r = calloc_checked(1, sizeof(Relocation));
      r->offset = (int)base_off;
      r->addend = addend;
      // Resolve to the gvar's name pointer so the relocation lives.
      for (Obj *o = globals; o; o = o->next) {
        if (!strcmp(o->name, lbl)) { r->label = &o->name; break; }
      }
      if (r->label) {
        *rh_tail = (*rh_tail)->next = r;
        return probe;
      }
      // Couldn't resolve the label (forward decl etc.) — fall through.
    }
    int64_t v = const_expr_val(&tok, tok);
    long sz = ty->size;
    for (long b = 0; b < sz && base_off + b < buf_sz; b++)
      buf[base_off + b] = (v >> (b * 8)) & 0xff;
    return tok;
  }

  int64_t v = const_expr_val(&tok, tok);
  long sz = ty->size;
  for (long b = 0; b < sz && base_off + b < buf_sz; b++)
    buf[base_off + b] = (v >> (b * 8)) & 0xff;
  return tok;
}

// make_offset_lval — build a fresh lvalue Node for `var` at byte
// `base_off`, with type `ty`.  Expands to `*(ty *)((char *)&var + off)`.
// Used by lvar_init_at_offset to avoid sharing AST subtrees across
// multiple assignments.
static Node *make_offset_lval(Obj *var, long base_off, Type *ty, Token *tok) {
  Node *vref = new_var_node(var, tok);
  Node *addr = new_unary(ND_ADDR, vref, tok);
  Node *cast_char = new_cast(addr, pointer_to(ty_char));
  Node *sum = new_add(cast_char, new_num(base_off, tok), tok);
  Node *cast_ty = new_cast(sum, pointer_to(ty));
  Node *deref = new_unary(ND_DEREF, cast_ty, tok);
  add_type(deref);
  return deref;
}

// lvar_init_at_offset — recursive local-variable initializer.
// Emits ND_ASSIGN/COMMA chain that initializes the region of `var`
// at `base_off` of type `ty` from the initializer at `tok`.  Returns
// the new chain head.  Advances *rest.
//
// Caller is expected to have prepended an ND_MEMZERO to clear the
// whole storage so omitted elements / brace gaps remain zero.
//
// Subset:
//   - Empty `{}` → no-op (memzero already covers it).
//   - char[] = "literal" → byte-by-byte assignment.
//   - TY_STRUCT brace with optional `.field` designators.
//   - TY_UNION brace with first-member or `.field`.
//   - TY_ARRAY brace, element-wise recursion.
//   - Scalar leaves (int / ptr / flonum) → `lval = expr`.
static Node *lvar_init_at_offset(Token **rest, Token *tok, Type *ty,
                                  Obj *var, long base_off, Node *chain,
                                  Token *eq) {
  // Empty brace → no-op (ND_MEMZERO already cleared).
  if (equal(tok, "{") && equal(tok->next, "}")) {
    *rest = tok->next->next;
    return chain;
  }

  concat_adjacent_strings(tok);
  // String literal → char-array byte-by-byte init.
  if (tok->kind == TK_STR && ty->kind == TY_ARRAY &&
      ty->base->kind == TY_CHAR) {
    long s = tok->ty->array_len;
    long n = ty->array_len > 0 && ty->array_len < s ? ty->array_len : s;
    for (long i = 0; i < n; i++) {
      Node *lv = make_offset_lval(var, base_off + i, ty_char, eq);
      Node *val = new_num((unsigned char)tok->str[i], eq);
      Node *as = new_binary(ND_ASSIGN, lv, val, eq);
      chain = new_binary(ND_COMMA, chain, as, eq);
    }
    *rest = tok->next;
    return chain;
  }

  // Brace-init aggregate.
  if (equal(tok, "{")) {
    tok = tok->next;
    if (ty->kind == TY_STRUCT) {
      Member *m = ty->members;
      bool first = true;
      while (!equal(tok, "}")) {
        if (!first) tok = skip(tok, ",");
        first = false;
        if (equal(tok, "}")) break;
        if (equal(tok, ".")) {
          tok = tok->next;
          if (tok->kind != TK_IDENT)
            error_tok(tok, "expected member name");
          Member *target = find_member(ty, tok);
          if (!target) error_tok(tok, "no such member");
          tok = skip(tok->next, "=");
          m = target;
        }
        if (!m) error_tok(tok, "excess elements in struct initializer");
        chain = lvar_init_at_offset(&tok, tok, m->ty, var,
                                     base_off + m->offset, chain, eq);
        m = m->next;
      }
      *rest = skip(tok, "}");
      return chain;
    }
    if (ty->kind == TY_UNION) {
      Member *m = ty->members;
      if (equal(tok, ".")) {
        tok = tok->next;
        if (tok->kind != TK_IDENT)
          error_tok(tok, "expected member name");
        Member *target = find_member(ty, tok);
        if (!target) error_tok(tok, "no such member");
        tok = skip(tok->next, "=");
        m = target;
      }
      if (m && !equal(tok, "}"))
        chain = lvar_init_at_offset(&tok, tok, m->ty, var,
                                     base_off + m->offset, chain, eq);
      if (equal(tok, ",")) tok = tok->next;
      *rest = skip(tok, "}");
      return chain;
    }
    if (ty->kind == TY_ARRAY) {
      long elem_sz = ty->base->size;
      long count = 0;
      bool first = true;
      while (!equal(tok, "}")) {
        if (!first) tok = skip(tok, ",");
        first = false;
        if (equal(tok, "}")) break;
        if (ty->array_len > 0 && count >= ty->array_len)
          break;
        chain = lvar_init_at_offset(&tok, tok, ty->base, var,
                                     base_off + count * elem_sz, chain, eq);
        count++;
      }
      *rest = skip(tok, "}");
      return chain;
    }
    // Brace around a scalar — `T x = { expr };`.
    Node *e = assign(&tok, tok);
    add_type(e);
    Node *lv = make_offset_lval(var, base_off, ty, eq);
    Node *as = new_binary(ND_ASSIGN, lv, e, eq);
    chain = new_binary(ND_COMMA, chain, as, eq);
    if (equal(tok, ",")) tok = tok->next;
    *rest = skip(tok, "}");
    return chain;
  }

  // Plain scalar = expr.
  Node *e = assign(&tok, tok);
  add_type(e);
  Node *lv = make_offset_lval(var, base_off, ty, eq);
  Node *as = new_binary(ND_ASSIGN, lv, e, eq);
  chain = new_binary(ND_COMMA, chain, as, eq);
  *rest = tok;
  return chain;
}
