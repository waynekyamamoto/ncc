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
static int64_t const_expr_val(Token **rest, Token *tok);
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

    // Otherwise: a declaration.  is_typename guards garbage input
    // so the failure mode is a clear "expected declaration" rather
    // than declspec errors mid-token.
    if (!is_typename(tok))
      error_tok(tok, "parse_v2: expected declaration");

    VarAttr attr = {0};
    Type *basety = declspec(&tok, tok, &attr);

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
// terminated by `}`.  Bitfield, anonymous-member, flexible-array,
// per-member-attribute, and in-body _Static_assert paths deferred.
static Member *struct_members(Token **rest, Token *tok) {
  Member head = {0};
  Member *cur = &head;
  int idx = 0;
  while (!equal(tok, "}")) {
    VarAttr attr = {0};
    Type *basety = declspec(&tok, tok, &attr);
    bool first = true;
    while (!equal(tok, ";")) {
      if (!first) tok = skip(tok, ",");
      first = false;
      Type *ty = declarator(&tok, tok, basety);
      if (!ty->name)
        error_tok(tok, "struct member requires a name");
      if (equal(tok, ":"))
        error_tok(tok, "parse_v2: bitfields not yet implemented");
      if (equal(tok, "__attribute__"))
        tok = attribute_list(tok->next, ty, &attr);

      Member *m = calloc_checked(1, sizeof(Member));
      m->ty = ty;
      m->name = ty->name;
      m->tok = ty->name;
      m->idx = idx++;
      m->align = attr.align ? attr.align : ty->align;
      cur = cur->next = m;
    }
    tok = skip(tok, ";");
  }
  *rest = skip(tok, "}");
  return head.next;
}

// struct_layout — assign offsets and compute size + align.  No
// packed / bitfield support yet.
static void struct_layout(Type *ty) {
  long offset = 0;
  int max_align = 1;
  for (Member *m = ty->members; m; m = m->next) {
    int al = m->align ? m->align : m->ty->align;
    if (al < 1) al = 1;
    offset = align_to(offset, al);
    m->offset = offset;
    offset += m->ty->size;
    if (al > max_align) max_align = al;
  }
  ty->align = max_align;
  ty->size = align_to(offset, max_align);
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
  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

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
  ty->members = struct_members(&tok, tok);
  if (is_union)
    union_layout(ty);
  else
    struct_layout(ty);

  if (tag) {
    // Push only if not already in scope (avoid duplicate entries
    // when reusing an incomplete forward declaration).
    TagScope *ts = find_tag(tag);
    if (!ts)
      push_tag_scope(tag, ty);
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

// find_member — direct linear search; anonymous-member recursion
// deferred until anonymous-member support lands.
static Member *find_member(Type *ty, Token *name) {
  for (Member *m = ty->members; m; m = m->next) {
    if (m->name && name->len == m->name->len &&
        !strncmp(m->name->loc, name->loc, name->len))
      return m;
  }
  return NULL;
}

// struct_ref — wrap node in ND_MEMBER for `node . name`.  Caller
// must have applied ND_DEREF for `->`.
static Node *struct_ref(Node *node, Token *name) {
  add_type(node);
  if (node->ty->kind != TY_STRUCT && node->ty->kind != TY_UNION)
    error_tok(node->tok, "not a struct or union");
  Member *m = find_member(node->ty, name);
  if (!m)
    error_tok(name, "no such member");
  Node *n = new_unary(ND_MEMBER, node, name);
  n->member = m;
  return n;
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

static Type *typeof_specifier(Token **rest, Token *tok) {
  (void)rest;
  error_tok(tok, "parse_v2: typeof not yet implemented");
}

static Type *atomic_specifier(Token **rest, Token *tok) {
  (void)rest;
  error_tok(tok, "parse_v2: _Atomic(type-name) not yet implemented");
}

static Token *parse_alignas(Token *tok, VarAttr *attr) {
  (void)attr;
  error_tok(tok, "parse_v2: _Alignas not yet implemented");
}

static Token *attribute_list(Token *tok, Type *ty, VarAttr *attr) {
  (void)ty; (void)attr;
  error_tok(tok, "parse_v2: attribute_list not yet implemented");
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

  // §F.5 — body.
  tok = skip(tok, "{");
  Node *body = compound_stmt(&tok, tok);
  add_type(body);

  // Resolve labels (§E.5).  No labels possible yet without goto/label
  // statements, but the walk is harmless.
  for (Node *g = gotos; g; g = g->goto_next) {
    if (g->kind == ND_LABEL_VAL) continue;
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
      error_tok(tok, "parse_v2: global initializer not yet implemented");
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

// skip_static_assert — stub that errors.  Real path evaluates the
// const-expr via try_eval_node and emits the message on failure.
static Token *skip_static_assert(Token *tok) {
  error_tok(tok, "parse_v2: _Static_assert not yet implemented");
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

  // goto IDENT ; (04c §E.1, direct form).
  if (equal(tok, "goto")) {
    if (tok->next->kind != TK_IDENT)
      error_tok(tok->next, "expected label");
    Node *node = new_node(ND_GOTO, tok);
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
      error_tok(tok, "parse_v2: static local not yet implemented");
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
      // Scalar initializer fast path: `T x = expr;` lowers to an
      // ND_EXPR_STMT(ND_ASSIGN(var, expr)).  Brace-form, struct, and
      // array initializers route through lvar_initializer (deferred).
      if (equal(tok->next, "{"))
        error_tok(tok, "parse_v2: brace initializer not yet implemented");
      Token *eq = tok;
      Node *lhs = new_var_node(var, ty->name);
      Node *rhs = assign(&tok, tok->next);
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

  // ptr + int — scale by element size.
  long elem_size = lhs->ty->base->size;
  rhs = new_binary(ND_MUL, rhs, new_num(elem_size, tok), tok);
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

  // ptr - int.
  if (lhs->ty->base && is_integer(rhs->ty)) {
    long elem_size = lhs->ty->base->size;
    rhs = new_binary(ND_MUL, rhs, new_num(elem_size, tok), tok);
    add_type(rhs);
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = lhs->ty;
    return node;
  }

  // ptr - ptr.
  if (lhs->ty->base && rhs->ty->base) {
    long elem_size = lhs->ty->base->size;
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = ty_long;
    return new_binary(ND_DIV, node, new_num(elem_size, tok), tok);
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

  // Compound assignment: pick the kind, build, lower.
  static const struct { const char *op; NodeKind kind; } cops[] = {
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
  for (size_t i = 0; i < sizeof(cops) / sizeof(*cops); i++) {
    if (equal(tok, cops[i].op)) {
      Token *op = tok;
      Node *rhs = assign(&tok, tok->next);
      Node *binary;
      if (cops[i].kind == ND_ADD)
        binary = new_add(node, rhs, op);
      else if (cops[i].kind == ND_SUB)
        binary = new_sub(node, rhs, op);
      else
        binary = new_binary(cops[i].kind, node, rhs, op);
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
  Node *op_node;
  if (binary->kind == ND_ADD)
    op_node = new_add(deref_r, binary->rhs, tok);
  else if (binary->kind == ND_SUB)
    op_node = new_sub(deref_r, binary->rhs, tok);
  else
    op_node = new_binary(binary->kind, deref_r, binary->rhs, tok);
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
//   - followed by `{` → compound literal (deferred).
//   - otherwise → cast: parse cast operand, wrap in ND_CAST.
//   - `( expr )` → fall through to unary→postfix→primary's parens.
static Node *cast(Token **rest, Token *tok) {
  if (equal(tok, "(") && is_typename(tok->next)) {
    Token *open = tok;
    Type *ty = typename_(&tok, tok->next);
    tok = skip(tok, ")");
    if (equal(tok, "{")) {
      // Compound literal — deferred.  Fall back to a unary parse on
      // the original `(` so primary handles the brace... actually
      // primary doesn't handle `{` either.  Surface a clear error
      // until compound-literal lands.
      error_tok(open, "parse_v2: compound literals not yet implemented");
    }
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

  // §G.6 — bitwise not.
  if (equal(tok, "~")) {
    Token *op = tok;
    Node *operand = cast(rest, tok->next);
    return new_unary(ND_BITNOT, operand, op);
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

// postfix — 04b §H suffix loop.  Subset:
//   primary [ expr ]
//   primary ( arg-list )
//   primary . ident       (deferred until struct lands)
//   primary -> ident      (deferred)
//   primary ++ / --       (deferred — needs to_assign)
static Node *postfix(Token **rest, Token *tok) {
  Node *node = primary(&tok, tok);

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
  // ( expr )
  if (equal(tok, "(")) {
    Node *node = expr(&tok, tok->next);
    *rest = skip(tok, ")");
    return node;
  }

  // String literal.
  if (tok->kind == TK_STR) {
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

  // Identifier — variable reference or enum constant.
  if (tok->kind == TK_IDENT) {
    VarScope *vs = find_var(tok);
    if (!vs || (!vs->var && !vs->enum_ty))
      error_tok(tok, "undefined variable");
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
