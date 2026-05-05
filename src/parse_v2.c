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
static Node *expr(Token **rest, Token *tok);
static Node *assign(Token **rest, Token *tok);
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

#if 0  // unused until tag-zone fills.
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
#endif

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

static Node *new_num(int64_t val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  node->ty = ty_int;
  return node;
}

static char *new_unique_name(void) {
  return format(".L..%d", label_cnt++);
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

static Type *struct_decl(Token **rest, Token *tok) {
  (void)rest;
  error_tok(tok, "parse_v2: struct_decl not yet implemented");
}

static Type *union_decl(Token **rest, Token *tok) {
  (void)rest;
  error_tok(tok, "parse_v2: union_decl not yet implemented");
}

static Type *enum_specifier(Token **rest, Token *tok) {
  (void)rest;
  error_tok(tok, "parse_v2: enum_specifier not yet implemented");
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

static Token *parse_typedef(Token *tok, Type *basety) {
  (void)basety;
  error_tok(tok, "parse_v2: parse_typedef not yet implemented");
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

  // §F.2 — register parameters (declared order: walk the param Type
  // chain front-to-back, build Obj in reverse via new_lvar so locals
  // ends up in declaration order).
  Obj *param_head = NULL;
  for (Type *pt = ty->params; pt; pt = pt->next) {
    char *pname;
    if (pt->name && pt->name->len > 0)
      pname = strndup_checked(pt->name->loc, pt->name->len);
    else
      pname = new_unique_name();
    Obj *p = new_lvar(pname, pt);
    // Splice into a separate params chain — locals already holds it
    // via new_lvar, but the Obj.params field expects *only* the
    // parameter Objs in declaration order.
    p->next = NULL;
    (void)p;
    // We rebuild fn->params after the loop from `locals` (which is
    // now in declaration order because we walked params front-to-back
    // and prepended).  Wait — new_lvar prepends, so locals is in
    // reverse-declaration order.  Reverse it below.
    (void)param_head;
  }
  // Reverse `locals` so it's in declaration order, then assign as
  // fn->params.  After this, additional locals from the body will be
  // prepended; fn->locals is set at the end of body parsing.
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

// array_dimensions — stub.  §D detail (constant vs VLA, leading
// qualifiers, multi-dimensional) requires expression parsing.
static Type *array_dimensions(Token **rest, Token *tok, Type *ty) {
  (void)rest; (void)ty;
  error_tok(tok, "parse_v2: array_dimensions not yet implemented");
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

// compound_stmt — `{` already consumed by caller.  Parses statements
// until `}` and returns an ND_BLOCK.  Currently only dispatches to
// stmt() — local declarations and other branches land in later
// commits.
static Node *compound_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_BLOCK, tok);
  Node head = {0};
  Node *cur = &head;

  enter_scope();
  while (!equal(tok, "}")) {
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

  // Block.
  if (equal(tok, "{"))
    return compound_stmt(rest, tok->next);

  error_tok(tok, "parse_v2: unsupported statement (slice incomplete)");
}

//
// Expression zone (04b_expr.md) — vertical-slice subset.
//

// expr → assign  (no comma operator yet)
static Node *expr(Token **rest, Token *tok) {
  return assign(rest, tok);
}

// assign → primary  (no operator ladder yet)
static Node *assign(Token **rest, Token *tok) {
  return primary(rest, tok);
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
