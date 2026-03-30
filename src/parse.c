// parse.c — Full C recursive-descent parser
// Builds a typed AST from preprocessed tokens.
#include "cc.h"

//
// Scope management
//

typedef struct VarScope VarScope;
struct VarScope {
  VarScope *next;
  char *name;
  Obj *var;
  Type *type_def;
  Type *enum_ty;
  int enum_val;
};

typedef struct TagScope TagScope;
struct TagScope {
  TagScope *next;
  char *name;
  Type *ty;
};

struct Scope {
  Scope *next;
  VarScope *vars;
  TagScope *tags;
};

// Scoped object list for break/continue targets
typedef struct {
  char *brk;
  char *cont;
  Node *current_switch;
} BreakContext;

static Obj *locals;
static Obj *globals;
static Scope *scope;
static Obj *current_fn;
static Node *gotos;
static Node *labels;
// static BreakContext brk_ctx;
static char *brk_label;
static char *cont_label;
static Node *current_switch;
int label_cnt;

static void enter_scope(void) {
  Scope *sc = calloc_checked(1, sizeof(Scope));
  sc->next = scope;
  scope = sc;
}

static void leave_scope(void) {
  scope = scope->next;
}

// Find variable in scope
static VarScope *find_var(Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->next) {
    for (VarScope *vs = sc->vars; vs; vs = vs->next) {
      if ((int)strlen(vs->name) == tok->len && !strncmp(tok->loc, vs->name, tok->len))
        return vs;
    }
  }
  return NULL;
}

// Find tag in scope
static Type *find_tag(Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->next)
    for (TagScope *ts = sc->tags; ts; ts = ts->next)
      if ((int)strlen(ts->name) == tok->len && !strncmp(tok->loc, ts->name, tok->len))
        return ts->ty;
  return NULL;
}

static void push_tag_scope(Token *tok, Type *ty) {
  TagScope *ts = calloc_checked(1, sizeof(TagScope));
  ts->name = strndup_checked(tok->loc, tok->len);
  ts->ty = ty;
  ts->next = scope->tags;
  scope->tags = ts;
}

static VarScope *push_scope(char *name) {
  VarScope *vs = calloc_checked(1, sizeof(VarScope));
  vs->name = name;
  vs->next = scope->vars;
  scope->vars = vs;
  return vs;
}

// Create a new unique name for labels
static char *new_unique_name(void) {
  return format(".L..%d", label_cnt++);
}

//
// Node construction
//

static Node *new_node(NodeKind kind, Token *tok) {
  Node *node = calloc_checked(1, sizeof(Node));
  node->kind = kind;
  node->tok = tok;
  return node;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

static Node *new_unary(NodeKind kind, Node *expr, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = expr;
  return node;
}

static Node *new_num(int64_t val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  return node;
}

static Node *new_long(int64_t val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  node->ty = ty_long;
  return node;
}

static Node *new_ulong(long val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  node->ty = ty_ulong;
  return node;
}

static Node *new_var_node(Obj *var, Token *tok) {
  Node *node = new_node(ND_VAR, tok);
  node->var = var;
  return node;
}

// Forward declarations for complex number helpers
static Node *new_real(Node *expr, Token *tok);
static Node *new_imag(Node *expr, Token *tok);
static Node *new_complex_val(Node *real_part, Node *imag_part, Type *cty, Token *tok);
static Node *new_complex_mul(Node *lhs, Node *rhs, Token *tok);
static Node *new_complex_div(Node *lhs, Node *rhs, Token *tok);

Node *new_cast(Node *expr, Type *ty) {
  add_type(expr);

  // Cast to complex from non-complex: create complex with real=expr, imag=0
  if (ty->kind == TY_COMPLEX && expr->ty->kind != TY_COMPLEX) {
    Node *zero = new_node(ND_NUM, expr->tok);
    zero->fval = 0.0;
    zero->val = 0;
    zero->ty = ty->base;
    // Cast the expression to the base type first
    Node *real_part = new_cast(expr, ty->base);
    return new_complex_val(real_part, zero, ty, expr->tok);
  }

  // Cast from complex to non-complex: extract real part and cast
  if (ty->kind != TY_COMPLEX && expr->ty->kind == TY_COMPLEX) {
    Node *real_part = new_real(expr, expr->tok);
    return new_cast(real_part, ty);
  }

  // Cast between complex types: cast each part
  if (ty->kind == TY_COMPLEX && expr->ty->kind == TY_COMPLEX) {
    if (ty->base->kind == expr->ty->base->kind)
      return expr;  // same base type, no-op
    Node *real_part = new_cast(new_real(expr, expr->tok), ty->base);
    Node *imag_part = new_cast(new_imag(expr, expr->tok), ty->base);
    return new_complex_val(real_part, imag_part, ty, expr->tok);
  }

  Node *node = calloc_checked(1, sizeof(Node));
  node->kind = ND_CAST;
  node->tok = expr->tok;
  node->lhs = expr;
  node->ty = copy_type(ty);
  return node;
}

// Helper: create a __real__ (ND_REAL) node for a complex expression
static Node *new_real(Node *expr, Token *tok) {
  Node *node = new_unary(ND_REAL, expr, tok);
  add_type(node);
  return node;
}

// Helper: create a __imag__ (ND_IMAG) node for a complex expression
static Node *new_imag(Node *expr, Token *tok) {
  Node *node = new_unary(ND_IMAG, expr, tok);
  add_type(node);
  return node;
}

//
// Variable creation
//

static Obj *new_var(char *name, Type *ty) {
  Obj *var = calloc_checked(1, sizeof(Obj));
  var->name = name;
  var->ty = ty;
  var->align = ty->align;
  push_scope(name)->var = var;
  return var;
}

static Obj *new_lvar(char *name, Type *ty) {
  Obj *var = new_var(name, ty);
  var->is_local = true;
  var->next = locals;
  locals = var;
  return var;
}

// Create a complex value from real and imaginary parts by allocating
// an anonymous local variable, assigning each part, and returning the variable.
// Returns: (tmp.__real__ = real_part, tmp.__imag__ = imag_part, tmp)
static Node *new_complex_val(Node *real_part, Node *imag_part, Type *cty, Token *tok) {
  Obj *tmp = new_lvar("", cty);
  Node *tmp_var = new_var_node(tmp, tok);

  // tmp.__real__ = real_part
  Node *set_real = new_binary(ND_ASSIGN, new_real(tmp_var, tok), real_part, tok);
  // tmp.__imag__ = imag_part
  Node *set_imag = new_binary(ND_ASSIGN, new_imag(tmp_var, tok), imag_part, tok);
  // (set_real, set_imag, tmp)
  Node *comma1 = new_binary(ND_COMMA, set_real, set_imag, tok);
  Node *result = new_binary(ND_COMMA, comma1, new_var_node(tmp, tok), tok);
  return result;
}

static Obj *new_gvar(char *name, Type *ty) {
  Obj *var = new_var(name, ty);
  var->next = globals;
  var->is_static = false;
  var->is_definition = true;
  globals = var;
  return var;
}

static char *new_gvar_name(void) {
  static int cnt = 0;
  return format(".L.data.%d", cnt++);
}

static Obj *new_anon_gvar(Type *ty) {
  Obj *var = new_gvar(new_gvar_name(), ty);
  var->is_static = true;
  return var;
}

static Obj *new_string_literal(char *p, Type *ty) {
  Obj *var = new_anon_gvar(ty);
  var->init_data = p;
  return var;
}

// Variable attributes
typedef struct {
  bool is_typedef;
  bool is_static;
  bool is_extern;
  bool is_inline;
  bool is_tls;
  bool is_noreturn;
  int align;
} VarAttr;

// Helper: check if a type (or any of its members) contains a VLA
static bool type_has_vla(Type *ty) {
  if (ty->kind == TY_VLA)
    return true;
  if (ty->kind == TY_ARRAY)
    return type_has_vla(ty->base);
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    for (Member *mem = ty->members; mem; mem = mem->next)
      if (type_has_vla(mem->ty))
        return true;
  }
  return false;
}

// Compute VLA byte size at runtime.
// For a VLA type like int[n], creates a hidden local variable to hold
// the byte size (n * sizeof(int)), emits an assignment to compute it,
// and sets ty->vla_size to that variable.
// Also handles structs/unions that contain VLA members.
// Returns a statement node that must be inserted into the statement list,
// or NULL if the type does not involve VLAs.
static Node *compute_vla_size(Type *ty, Token *tok) {
  if (ty->kind == TY_VLA) {
    // Recursively compute base VLA sizes first (for multi-dimensional VLAs)
    Node *base_stmt = compute_vla_size(ty->base, tok);

    // Create hidden variable to hold byte size
    Obj *size_var = new_lvar("", ty_ulong);
    ty->vla_size = size_var;

    // Compute: vla_len * base_size_in_bytes
    Node *len_node = ty->vla_len;
    Node *base_size;
    if (ty->base->kind == TY_VLA)
      base_size = new_var_node(ty->base->vla_size, tok);
    else if (ty->base->vla_size)
      base_size = new_var_node(ty->base->vla_size, tok);
    else
      base_size = new_ulong(ty->base->size, tok);

    // Cast vla_len to unsigned long for the multiplication
    Node *len_cast = new_cast(len_node, ty_ulong);
    Node *mul_node = new_binary(ND_MUL, len_cast, base_size, tok);
    add_type(mul_node);

    Node *assign = new_binary(ND_ASSIGN, new_var_node(size_var, tok), mul_node, tok);
    add_type(assign);

    Node *stmt = new_node(ND_EXPR_STMT, tok);
    stmt->lhs = assign;

    if (base_stmt) {
      // Chain: first compute base size, then this size
      base_stmt->next = stmt;
      Node *block = new_node(ND_BLOCK, tok);
      block->body = base_stmt;
      return block;
    }

    return stmt;
  }

  // Handle structs/unions that contain VLA members
  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && type_has_vla(ty)) {
    // First, recursively compute VLA sizes for all members
    Node stmt_head = {};
    Node *stmt_cur = &stmt_head;

    for (Member *mem = ty->members; mem; mem = mem->next) {
      Node *mem_stmt = compute_vla_size(mem->ty, tok);
      if (mem_stmt)
        stmt_cur = stmt_cur->next = mem_stmt;
    }

    // Create hidden variable for the struct's runtime size
    Obj *size_var = new_lvar("", ty_ulong);
    ty->vla_size = size_var;

    // For a struct: sum of member sizes (simplified: last member offset + size)
    // For a union: max of member sizes
    // Build runtime expression to compute the total size.
    // We approximate: for structs, compute sum with alignment padding;
    // for simplicity, compute offset + size of last member at runtime.
    Node *total_size = NULL;
    if (ty->kind == TY_STRUCT) {
      // Runtime struct size = sum of all member sizes with alignment
      // For each member, we know the compile-time offset (from struct_layout).
      // The last member that is VLA determines the runtime portion.
      // Actually, for structs with VLA members, the layout is:
      //   fixed members at compile-time offsets
      //   VLA member's offset is known at compile time, but its size is runtime
      // So: struct_size = last_vla_member_offset + runtime_vla_member_size
      Member *last = NULL;
      for (Member *mem = ty->members; mem; mem = mem->next)
        last = mem;

      if (last && last->ty->vla_size) {
        // size = last->offset + vla_size_of_last_member
        Node *offset_node = new_ulong(last->offset, tok);
        Node *last_size = new_var_node(last->ty->vla_size, tok);
        total_size = new_binary(ND_ADD, offset_node, last_size, tok);
        add_type(total_size);
      } else if (last && type_has_vla(last->ty)) {
        total_size = new_ulong(ty->size, tok);
      } else {
        total_size = new_ulong(ty->size, tok);
      }
    } else {
      // Union: max of member sizes (simplified for now)
      total_size = new_ulong(ty->size, tok);
      for (Member *mem = ty->members; mem; mem = mem->next) {
        if (mem->ty->vla_size) {
          total_size = new_var_node(mem->ty->vla_size, tok);
          break;
        }
      }
    }

    Node *assign = new_binary(ND_ASSIGN, new_var_node(size_var, tok), total_size, tok);
    add_type(assign);
    Node *assign_stmt = new_node(ND_EXPR_STMT, tok);
    assign_stmt->lhs = assign;
    stmt_cur = stmt_cur->next = assign_stmt;

    if (stmt_head.next) {
      if (stmt_head.next->next) {
        Node *block = new_node(ND_BLOCK, tok);
        block->body = stmt_head.next;
        return block;
      }
      return stmt_head.next;
    }
    return NULL;
  }

  return NULL;
}

//
// Type handling utilities
//

static bool is_typename(Token *tok);
static Type *declspec(Token **rest, Token *tok, VarAttr *attr);
static Type *typename_(Token **rest, Token *tok);
static Type *declarator(Token **rest, Token *tok, Type *ty);
static Type *abstract_declarator(Token **rest, Token *tok, Type *ty);

// Forward declarations for expression/statement parsing
static Node *compound_stmt(Token **rest, Token *tok);
static Node *stmt(Token **rest, Token *tok);
static Node *expr_stmt(Token **rest, Token *tok);
static Node *expr(Token **rest, Token *tok);
static Node *assign(Token **rest, Token *tok);
static Node *cond_expr(Token **rest, Token *tok);
static Node *logor(Token **rest, Token *tok);
static Node *logand(Token **rest, Token *tok);
static Node *bitor_(Token **rest, Token *tok);
static Node *bitxor(Token **rest, Token *tok);
static Node *bitand(Token **rest, Token *tok);
static Node *equality(Token **rest, Token *tok);
static Node *relational(Token **rest, Token *tok);
static Node *shift(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok);
static Node *mul(Token **rest, Token *tok);
static Node *cast(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *postfix(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);
static Node *funcall(Token **rest, Token *tok, Node *fn);
static Type *struct_decl(Token **rest, Token *tok);
static Type *union_decl(Token **rest, Token *tok);
static Type *enum_specifier(Token **rest, Token *tok);
static void struct_members(Token **rest, Token *tok, Type *ty);
static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr);
static Token *function(Token *tok, Type *basety, VarAttr *attr);
static Token *global_variable(Token *tok, Type *ty, Type *basety, VarAttr *attr);
static int64_t const_expr_val(Token **rest, Token *tok);
static int64_t eval(Node *node);
static int64_t eval2(Node *node, char ***label);
static int64_t eval_rval(Node *node, char ***label);
static double eval_double(Node *node);
static void gvar_initializer(Token **rest, Token *tok, Obj *var);
static Node *lvar_initializer(Token **rest, Token *tok, Obj *var);
static Node *to_assign(Node *binary);
static Node *new_add(Node *lhs, Node *rhs, Token *tok);
static Node *new_sub(Node *lhs, Node *rhs, Token *tok);
static Node *new_inc_dec(Node *node, Token *tok, int addend);
static Token *parse_typedef(Token *tok, Type *basety, Node **cur);
static bool is_function_definition(Token *tok, Type *basety);
static Token *attribute_list(Token *tok, Type *ty, VarAttr *attr);
static bool is_end(Token *tok);
typedef struct InitDesig InitDesig;
static Node *init_desig_expr(InitDesig *desig, Token *tok);

//
// Determine if a token starts a type name
//

static bool is_typename(Token *tok) {
  static const char *kw[] = {
    "void", "_Bool", "char", "short", "int", "long", "float", "double",
    "struct", "union", "enum", "typedef", "static", "extern", "inline",
    "_Noreturn", "signed", "unsigned", "const", "volatile", "restrict",
    "_Atomic", "_Alignas", "auto", "register", "_Thread_local", "__thread",
    "typeof", "__typeof__", "_Static_assert", "static_assert",
    "__extension__", "__builtin_va_list", "__attribute__",
    "_Complex", "__complex__",
  };

  for (int i = 0; i < (int)(sizeof(kw) / sizeof(*kw)); i++)
    if (equal(tok, kw[i]))
      return true;

  // Check for typedef names
  VarScope *vs = find_var(tok);
  return vs && vs->type_def;
}

//
// Declaration specifiers (type + storage class + qualifiers)
//

static Type *declspec(Token **rest, Token *tok, VarAttr *attr) {
  // Count type specifiers
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

  Type *ty = ty_int;
  int counter = 0;
  bool is_atomic = false;
  bool is_const = false;
  bool is_volatile = false;

  while (is_typename(tok)) {
    // Storage class specifiers
    if (equal(tok, "typedef") || equal(tok, "static") || equal(tok, "extern") ||
        equal(tok, "inline") || equal(tok, "_Noreturn") || equal(tok, "register") ||
        equal(tok, "_Thread_local") || equal(tok, "__thread")) {
      if (attr) {
        if (equal(tok, "typedef"))      attr->is_typedef = true;
        else if (equal(tok, "static"))  attr->is_static = true;
        else if (equal(tok, "extern"))  attr->is_extern = true;
        else if (equal(tok, "inline"))  attr->is_inline = true;
        else if (equal(tok, "_Noreturn")) attr->is_noreturn = true;
        else if (equal(tok, "_Thread_local") || equal(tok, "__thread"))
          attr->is_tls = true;
      }
      // "register" and "auto" are ignored
      tok = tok->next;
      continue;
    }

    // __extension__ — silently consume (no-op)
    if (equal(tok, "__extension__")) {
      tok = tok->next;
      continue;
    }

    // __attribute__((...)) — consume in declspec context
    if (equal(tok, "__attribute__")) {
      tok = attribute_list(tok, ty, attr);
      continue;
    }

    // Type qualifiers
    if (equal(tok, "const") || equal(tok, "volatile") || equal(tok, "restrict") ||
        equal(tok, "__restrict") || equal(tok, "__restrict__")) {
      if (equal(tok, "const")) is_const = true;
      if (equal(tok, "volatile")) is_volatile = true;
      tok = tok->next;
      continue;
    }

    if (equal(tok, "_Atomic")) {
      tok = tok->next;
      if (equal(tok, "(")) {
        ty = typename_(&tok, tok->next);
        tok = skip(tok, ")");
      }
      is_atomic = true;
      continue;
    }

    if (equal(tok, "_Alignas")) {
      if (!attr)
        error_tok(tok, "_Alignas is not allowed here");
      tok = skip(tok->next, "(");
      if (is_typename(tok)) {
        Type *t = typename_(&tok, tok);
        attr->align = t->align;
      } else {
        attr->align = const_expr_val(&tok, tok);
      }
      tok = skip(tok, ")");
      continue;
    }

    // typeof
    if (equal(tok, "typeof") || equal(tok, "__typeof__")) {
      tok = skip(tok->next, "(");
      if (is_typename(tok)) {
        ty = typename_(&tok, tok);
      } else {
        Node *node = expr(&tok, tok);
        add_type(node);
        ty = node->ty;
      }
      tok = skip(tok, ")");
      counter += OTHER;
      continue;
    }

    // __builtin_va_list — treat as void *
    if (equal(tok, "__builtin_va_list")) {
      ty = pointer_to(ty_void);
      tok = tok->next;
      counter += OTHER;
      continue;
    }

    // _Static_assert
    if (equal(tok, "_Static_assert") || equal(tok, "static_assert")) {
      tok = skip(tok->next, "(");
      int64_t val = const_expr_val(&tok, tok);
      tok = skip(tok, ",");
      // Skip the message string
      if (tok->kind == TK_STR)
        tok = tok->next;
      tok = skip(tok, ")");
      tok = skip(tok, ";");
      if (!val)
        error_tok(tok, "static assertion failed");
      continue;
    }

    // Type specifiers
    if (equal(tok, "struct")) {
      ty = struct_decl(&tok, tok->next);
      counter += OTHER;
      continue;
    }
    if (equal(tok, "union")) {
      ty = union_decl(&tok, tok->next);
      counter += OTHER;
      continue;
    }
    if (equal(tok, "enum")) {
      ty = enum_specifier(&tok, tok->next);
      counter += OTHER;
      continue;
    }

    // Handle user-defined type names (typedef)
    if (counter == 0) {
      VarScope *vs = find_var(tok);
      if (vs && vs->type_def) {
        ty = vs->type_def;
        counter += OTHER;
        tok = tok->next;
        continue;
      }
    }

    if (equal(tok, "void"))         counter += VOID;
    else if (equal(tok, "_Bool"))   counter += BOOL;
    else if (equal(tok, "char"))    counter += CHAR;
    else if (equal(tok, "short"))   counter += SHORT;
    else if (equal(tok, "int"))     counter += INT;
    else if (equal(tok, "long"))    counter += LONG;
    else if (equal(tok, "float"))   counter += FLOAT;
    else if (equal(tok, "double"))  counter += DOUBLE;
    else if (equal(tok, "signed"))  counter |= SIGNED;
    else if (equal(tok, "unsigned")) counter |= UNSIGNED;
    else if (equal(tok, "_Complex") || equal(tok, "__complex__"))
      counter |= COMPLEX;
    else
      break;

    switch (counter) {
    case VOID:                            ty = ty_void; break;
    case BOOL:                            ty = ty_bool; break;
    case CHAR: case SIGNED + CHAR:        ty = ty_char; break;
    case UNSIGNED + CHAR:                 ty = ty_uchar; break;
    case SHORT: case SHORT + INT:
    case SIGNED + SHORT:
    case SIGNED + SHORT + INT:            ty = ty_short; break;
    case UNSIGNED + SHORT:
    case UNSIGNED + SHORT + INT:          ty = ty_ushort; break;
    case INT: case SIGNED:
    case SIGNED + INT:                    ty = ty_int; break;
    case UNSIGNED: case UNSIGNED + INT:   ty = ty_uint; break;
    case LONG: case LONG + INT:
    case SIGNED + LONG:
    case SIGNED + LONG + INT:             ty = ty_long; break;
    case UNSIGNED + LONG:
    case UNSIGNED + LONG + INT:           ty = ty_ulong; break;
    case LONG + LONG: case LONG + LONG + INT:
    case SIGNED + LONG + LONG:
    case SIGNED + LONG + LONG + INT:      ty = ty_longlong; break;
    case UNSIGNED + LONG + LONG:
    case UNSIGNED + LONG + LONG + INT:    ty = ty_ulonglong; break;
    case FLOAT:                           ty = ty_float; break;
    case DOUBLE:                          ty = ty_double; break;
    case LONG + DOUBLE:                   ty = ty_ldouble; break;
    // _Complex types
    case COMPLEX:
    case COMPLEX + DOUBLE:                ty = complex_type(ty_double); break;
    case COMPLEX + FLOAT:                 ty = complex_type(ty_float); break;
    case COMPLEX + LONG + DOUBLE:         ty = complex_type(ty_ldouble); break;
    case COMPLEX + INT:
    case COMPLEX + SIGNED:
    case COMPLEX + SIGNED + INT:          ty = complex_type(ty_int); break;
    case COMPLEX + UNSIGNED:
    case COMPLEX + UNSIGNED + INT:        ty = complex_type(ty_uint); break;
    case COMPLEX + LONG:
    case COMPLEX + LONG + INT:
    case COMPLEX + SIGNED + LONG:
    case COMPLEX + SIGNED + LONG + INT:   ty = complex_type(ty_long); break;
    case COMPLEX + UNSIGNED + LONG:
    case COMPLEX + UNSIGNED + LONG + INT: ty = complex_type(ty_ulong); break;
    case COMPLEX + CHAR:
    case COMPLEX + SIGNED + CHAR:         ty = complex_type(ty_char); break;
    case COMPLEX + UNSIGNED + CHAR:       ty = complex_type(ty_uchar); break;
    default:
      error_tok(tok, "invalid type");
    }

    tok = tok->next;
  }

  if (is_atomic || is_const || is_volatile) {
    ty = copy_type(ty);
    if (is_atomic) ty->is_atomic = true;
    if (is_const) ty->is_const = true;
    if (is_volatile) ty->is_volatile = true;
  }

  *rest = tok;
  return ty;
}

//
// Declarator: handles *, [], (), identifier parts of a declaration
//

// pointer
static Type *pointers(Token **rest, Token *tok, Type *ty) {
  while (consume(&tok, tok, "*")) {
    ty = pointer_to(ty);
    bool pconst = false, pvolatile = false;
    while (equal(tok, "const") || equal(tok, "volatile") ||
           equal(tok, "restrict") || equal(tok, "__restrict") ||
           equal(tok, "__restrict__") || equal(tok, "_Atomic")) {
      if (equal(tok, "const")) pconst = true;
      if (equal(tok, "volatile")) pvolatile = true;
      tok = tok->next;
    }
    if (pconst) ty->is_const = true;
    if (pvolatile) ty->is_volatile = true;
  }
  *rest = tok;
  return ty;
}

// type-suffix: handles array dimensions and function parameters
static Type *type_suffix(Token **rest, Token *tok, Type *ty);

// func-params
static Type *func_params(Token **rest, Token *tok, Type *ty) {
  // Check for old-style (K&R) empty params
  if (equal(tok, ")")) {
    *rest = tok->next;
    return func_type(ty);
  }

  // Check for (void)
  if (equal(tok, "void") && equal(tok->next, ")")) {
    *rest = tok->next->next;
    return func_type(ty);
  }

  Type head = {};
  Type *cur = &head;
  bool is_variadic = false;

  while (!equal(tok, ")")) {
    if (cur != &head)
      tok = skip(tok, ",");

    if (equal(tok, "...")) {
      is_variadic = true;
      tok = tok->next;
      break;
    }

    Type *ty2 = declspec(&tok, tok, NULL);
    ty2 = declarator(&tok, tok, ty2);

    // Consume trailing __attribute__ on parameters
    // e.g., int argc __attribute__((unused))
    tok = attribute_list(tok, ty2, NULL);

    Token *name = ty2->name;

    // Array parameters decay to pointers
    if (ty2->kind == TY_ARRAY) {
      ty2 = pointer_to(ty2->base);
      ty2->name = name;
    }
    // Function parameters decay to pointers
    if (ty2->kind == TY_FUNC) {
      ty2 = pointer_to(ty2);
      ty2->name = name;
    }

    Type *param = copy_type(ty2);
    param->next = NULL;
    cur = cur->next = param;
  }

  if (cur == &head)
    is_variadic = true; // f() = f(...) effectively

  tok = skip(tok, ")");

  ty = func_type(ty);
  ty->params = head.next;
  ty->is_variadic = is_variadic;
  *rest = tok;
  return ty;
}

// Array dimensions
static Type *array_dimensions(Token **rest, Token *tok, Type *ty) {
  // Handle [] (flexible/incomplete array)
  while (equal(tok, "static") || equal(tok, "const") || equal(tok, "volatile") ||
         equal(tok, "restrict"))
    tok = tok->next;

  if (equal(tok, "]")) {
    ty = type_suffix(rest, tok->next, ty);
    return array_of(ty, -1); // incomplete array
  }

  // Handle [N] or [expr]
  Node *expr_node = cond_expr(&tok, tok);
  tok = skip(tok, "]");
  ty = type_suffix(&tok, tok, ty);

  // Try to evaluate as constant expression
  add_type(expr_node);
  // Check if the expression is a simple constant (literal, or arithmetic on literals)
  // eval() would work but crashes on non-constant expressions.
  // Simple check: does the tree contain any ND_VAR or ND_FUNCALL?
  {
    bool is_const = true;
    // Walk the tree to check for non-constant nodes.
    // ND_ADDR, ND_MEMBER, and ND_DEREF are allowed because they appear
    // in offsetof() patterns like &((type*)0)->member, which are
    // compile-time constants.
    Node *stack[64];
    int sp = 0;
    stack[sp++] = expr_node;
    while (sp > 0 && is_const) {
      Node *n = stack[--sp];
      if (!n) continue;
      if (n->kind == ND_VAR || n->kind == ND_FUNCALL)
        is_const = false;
      if (sp < 62) {
        if (n->lhs) stack[sp++] = n->lhs;
        if (n->rhs) stack[sp++] = n->rhs;
      }
    }
    if (is_const) {
      int64_t val = eval(expr_node);
      *rest = tok;
      return array_of(ty, val);
    }
  }

  // VLA
  *rest = tok;
  return vla_of(ty, expr_node);
}

static Type *type_suffix(Token **rest, Token *tok, Type *ty) {
  if (equal(tok, "("))
    return func_params(rest, tok->next, ty);

  if (equal(tok, "["))
    return array_dimensions(rest, tok->next, ty);

  *rest = tok;
  return ty;
}

// declarator: parse a declarator
static Type *declarator(Token **rest, Token *tok, Type *ty) {
  ty = pointers(&tok, tok, ty);

  // Consume __attribute__ after pointer stars (e.g., void * __attribute__((noinline)) foo())
  tok = attribute_list(tok, ty, NULL);

  // Check for nested declarator: ( declarator )
  if (equal(tok, "(") && (tok->next->kind == TK_IDENT || equal(tok->next, "*") ||
                           equal(tok->next, "("))) {
    // Could be: (declarator) suffix, or (params)
    // We need to disambiguate. If what follows the matching ')' is [ or (,
    // it's a nested declarator.
    Token *start = tok;

    // Try parsing as nested
    Type dummy = {};
    Token *rest2;
    declarator(&rest2, start->next, &dummy);
    if (equal(rest2, ")") &&
        (equal(rest2->next, "[") || equal(rest2->next, "(") || equal(rest2->next, ")") ||
         equal(rest2->next, ",") || equal(rest2->next, ";") || equal(rest2->next, "=") ||
         equal(rest2->next, "__attribute__"))) {
      // It's a nested declarator
      tok = start->next;
      Type *placeholder = calloc_checked(1, sizeof(Type));
      Type *inner = declarator(&tok, tok, placeholder);
      tok = skip(tok, ")");
      *placeholder = *type_suffix(rest, tok, ty);
      return inner;
    }
  }

  // Identifier
  Token *name = NULL;
  Token *name_pos = tok;

  if (tok->kind == TK_IDENT) {
    name = tok;
    tok = tok->next;
  }

  ty = type_suffix(rest, tok, ty);
  // Copy to avoid corrupting shared type objects (like global ty_int)
  // But for struct/union/enum, don't copy — they need to share identity
  // for forward declarations to work
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION || ty->kind == TY_ENUM) {
    ty->name = name;
    ty->name_pos = name_pos;
  } else {
    ty = copy_type(ty);
    ty->name = name;
    ty->name_pos = name_pos;
  }
  return ty;
}

// abstract-declarator: like declarator but without an identifier
static Type *abstract_declarator(Token **rest, Token *tok, Type *ty) {
  ty = pointers(&tok, tok, ty);

  if (equal(tok, "(") && !is_typename(tok->next) && !equal(tok->next, ")")) {
    Token *start = tok;
    Type dummy = {};
    abstract_declarator(&tok, start->next, &dummy);
    tok = skip(tok, ")");
    Type *inner_ty = type_suffix(rest, tok, ty);

    Token *saved_rest = *rest;
    tok = start->next;
    Type *placeholder = calloc_checked(1, sizeof(Type));
    *placeholder = *inner_ty;
    Token *unused;
    Type *result = abstract_declarator(&unused, tok, placeholder);
    *rest = saved_rest;
    return result;
  }

  return type_suffix(rest, tok, ty);
}

// typename: used in casts, sizeof, etc.
static Type *typename_(Token **rest, Token *tok) {
  Type *ty = declspec(&tok, tok, NULL);
  return abstract_declarator(rest, tok, ty);
}

//
// Struct/union declarations
//

static void struct_members(Token **rest, Token *tok, Type *ty) {
  Member head = {};
  Member *cur = &head;
  int idx = 0;

  while (!equal(tok, "}")) {
    // Handle _Static_assert inside struct
    if (equal(tok, "_Static_assert") || equal(tok, "static_assert")) {
      tok = skip(tok->next, "(");
      const_expr_val(&tok, tok);
      tok = skip(tok, ",");
      if (tok->kind == TK_STR)
        tok = tok->next;
      tok = skip(tok, ")");
      tok = skip(tok, ";");
      continue;
    }

    VarAttr attr = {};
    Type *basety = declspec(&tok, tok, &attr);

    // Anonymous struct member
    if ((basety->kind == TY_STRUCT || basety->kind == TY_UNION) &&
        consume(&tok, tok, ";")) {
      Member *mem = calloc_checked(1, sizeof(Member));
      mem->ty = basety;
      mem->idx = idx++;
      mem->align = attr.align ? attr.align : basety->align;
      cur = cur->next = mem;
      continue;
    }

    bool first = true;
    while (!consume(&tok, tok, ";")) {
      if (!first)
        tok = skip(tok, ",");
      first = false;

      Member *mem = calloc_checked(1, sizeof(Member));

      // Check for bitfield
      Type *mem_ty = declarator(&tok, tok, basety);
      mem->name = mem_ty->name;
      mem->ty = mem_ty;
      mem->idx = idx++;
      mem->align = attr.align ? attr.align : mem_ty->align;

      if (consume(&tok, tok, ":")) {
        mem->is_bitfield = true;
        mem->bit_width = const_expr_val(&tok, tok);
      }

      // Handle __attribute__ on struct members
      tok = attribute_list(tok, mem->ty, NULL);

      cur = cur->next = mem;
    }
  }

  // Skip flexible array member check for now
  if (cur != &head && cur->ty->kind == TY_ARRAY && cur->ty->array_len < 0) {
    cur->ty = array_of(cur->ty->base, 0);
    ty->is_flexible = true;
  }

  *rest = tok->next;
  ty->members = head.next;
}

// Layout struct members (calculate offsets)
static void struct_layout(Type *ty) {
  int offset = 0;
  int max_align = 1;
  int bits = 0;

  for (Member *mem = ty->members; mem; mem = mem->next) {
    if (mem->is_bitfield) {
      int sz = mem->ty->size;

      // Sync bit counter with byte offset when transitioning
      // from non-bitfield to bitfield members
      if (bits == 0 && offset > 0)
        bits = offset * 8;

      if (mem->bit_width == 0) {
        // Zero-width: align to next unit boundary
        bits = align_to(bits, sz * 8);
        offset = bits / 8;
        continue;
      }

      // Check if bitfield fits in current unit
      int cur_unit_bits = (bits / (sz * 8) + 1) * sz * 8;
      if (bits + mem->bit_width > cur_unit_bits)
        bits = cur_unit_bits;

      mem->offset = bits / 8 / sz * sz;
      mem->bit_offset = bits % (sz * 8);
      bits += mem->bit_width;

      offset = (bits + 7) / 8;
      if (mem->align > max_align)
        max_align = mem->align;
      continue;
    }

    if (!ty->is_packed)
      offset = align_to(offset, mem->align);
    else
      bits = align_to(bits, 8);

    // Reset bit counter for non-bitfield members
    bits = 0;

    mem->offset = offset;
    offset += mem->ty->size;

    if (mem->align > max_align)
      max_align = mem->align;
  }

  ty->align = max_align;
  ty->size = align_to(offset, max_align);
}

// Layout union members
static void union_layout(Type *ty) {
  int max_align = 1;
  int max_size = 0;

  for (Member *mem = ty->members; mem; mem = mem->next) {
    mem->offset = 0;
    if (mem->align > max_align)
      max_align = mem->align;
    if (mem->ty->size > max_size)
      max_size = mem->ty->size;
  }

  ty->align = max_align;
  ty->size = align_to(max_size, max_align);
}

// Skip __asm__("...") label on a declaration
static Token *skip_asm_label(Token *tok) {
  if (equal(tok, "asm") || equal(tok, "__asm__") || equal(tok, "__asm")) {
    tok = tok->next;
    tok = skip(tok, "(");
    // Skip everything until matching )
    int level = 1;
    while (level > 0) {
      if (equal(tok, "(")) level++;
      else if (equal(tok, ")")) level--;
      if (level > 0) tok = tok->next;
    }
    tok = tok->next; // skip final )
  }
  return tok;
}

// Parse __attribute__((..))
static Token *attribute_list(Token *tok, Type *ty, VarAttr *attr) {
  tok = skip_asm_label(tok);
  while (consume(&tok, tok, "__attribute__")) {
    tok = skip(tok, "(");
    tok = skip(tok, "(");

    while (!equal(tok, ")")) {
      if (equal(tok, "packed") || equal(tok, "__packed__")) {
        if (ty) ty->is_packed = true;
        tok = tok->next;
      } else if (equal(tok, "aligned") || equal(tok, "__aligned__")) {
        tok = tok->next;
        if (consume(&tok, tok, "(")) {
          int a = const_expr_val(&tok, tok);
          tok = skip(tok, ")");
          if (ty) {
            ty->align = a;
            // Also round up size to alignment
            if (ty->size > 0)
              ty->size = align_to(ty->size, a);
          }
          if (attr) attr->align = a;
        } else {
          // __attribute__((aligned)) with no args means max alignment (16 on ARM64)
          int a = 16;
          if (ty) {
            ty->align = a;
            if (ty->size > 0)
              ty->size = align_to(ty->size, a);
          }
          if (attr) attr->align = a;
        }
      } else if (equal(tok, "section")) {
        tok = tok->next;
        tok = skip(tok, "(");
        if (tok->kind == TK_STR)
          tok = tok->next;
        tok = skip(tok, ")");
      } else if (equal(tok, "visibility")) {
        tok = tok->next;
        tok = skip(tok, "(");
        if (tok->kind == TK_STR)
          tok = tok->next;
        tok = skip(tok, ")");
      } else if (equal(tok, "unused") || equal(tok, "__unused__") ||
                 equal(tok, "weak") || equal(tok, "may_alias") ||
                 equal(tok, "noinline") || equal(tok, "noclone") ||
                 equal(tok, "noreturn") || equal(tok, "nothrow") ||
                 equal(tok, "pure") || equal(tok, "const") ||
                 equal(tok, "deprecated") || equal(tok, "used") ||
                 equal(tok, "warn_unused_result") ||
                 equal(tok, "always_inline") || equal(tok, "cold") ||
                 equal(tok, "hot") || equal(tok, "malloc") ||
                 equal(tok, "flatten") || equal(tok, "constructor") ||
                 equal(tok, "destructor") ||
                 equal(tok, "transparent_union") ||
                 equal(tok, "returns_nonnull")) {
        tok = tok->next;
        // Some attrs take arguments
        if (consume(&tok, tok, "(")) {
          int level = 1;
          while (level > 0) {
            if (equal(tok, "(")) level++;
            if (equal(tok, ")")) level--;
            if (level > 0) tok = tok->next;
          }
          tok = tok->next;
        }
      } else if (equal(tok, "format") || equal(tok, "__format__") ||
                 equal(tok, "sentinel") || equal(tok, "alloc_size") ||
                 equal(tok, "cleanup") || equal(tok, "nonnull") ||
                 equal(tok, "mode")) {
        tok = tok->next;
        tok = skip(tok, "(");
        int level = 1;
        while (level > 0) {
          if (equal(tok, "(")) level++;
          if (equal(tok, ")")) level--;
          if (level > 0) tok = tok->next;
        }
        tok = tok->next;
      } else {
        // Unknown attribute — skip it and any arguments
        tok = tok->next;
        if (consume(&tok, tok, "(")) {
          int level = 1;
          while (level > 0) {
            if (equal(tok, "(")) level++;
            if (equal(tok, ")")) level--;
            if (level > 0) tok = tok->next;
          }
          tok = tok->next;
        }
      }

      if (!equal(tok, ")"))
        tok = skip(tok, ",");
    }

    tok = skip(tok, ")");
    tok = skip(tok, ")");
  }
  return tok;
}

static Type *struct_decl(Token **rest, Token *tok) {
  Token *tag = NULL;

  // Collect attributes that appear before or after the tag name.
  // Use a temporary type to capture packed/aligned attributes.
  Type early_attr = {};
  tok = attribute_list(tok, &early_attr, NULL);

  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  // Attribute after tag name, before body
  tok = attribute_list(tok, &early_attr, NULL);

  if (tag && !equal(tok, "{")) {
    // Forward reference or existing tag
    *rest = tok;
    Type *ty = find_tag(tag);
    if (ty) return ty;

    ty = struct_type();
    ty->size = -1; // incomplete
    push_tag_scope(tag, ty);
    return ty;
  }

  tok = skip(tok, "{");

  Type *ty = NULL;
  if (tag)
    ty = find_tag(tag);
  if (!tag || !ty || ty->size >= 0) {
    ty = struct_type();
    if (tag)
      push_tag_scope(tag, ty);
  }

  // Apply early attributes (e.g., packed from before the tag name)
  if (early_attr.is_packed)
    ty->is_packed = true;
  if (early_attr.align)
    ty->align = early_attr.align;

  struct_members(&tok, tok, ty);
  struct_layout(ty);
  // Apply attributes AFTER layout (so aligned() overrides computed alignment)
  tok = attribute_list(tok, ty, NULL);
  *rest = tok;
  return ty;
}

static Type *union_decl(Token **rest, Token *tok) {
  Token *tag = NULL;

  // Collect early attributes
  Type early_attr = {};
  tok = attribute_list(tok, &early_attr, NULL);

  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  tok = attribute_list(tok, &early_attr, NULL);

  if (tag && !equal(tok, "{")) {
    *rest = tok;
    Type *ty = find_tag(tag);
    if (ty) return ty;

    ty = struct_type();
    ty->kind = TY_UNION;
    ty->size = -1;
    push_tag_scope(tag, ty);
    return ty;
  }

  tok = skip(tok, "{");

  Type *ty = NULL;
  if (tag) ty = find_tag(tag);
  if (!tag || !ty || ty->size >= 0) {
    ty = struct_type();
    ty->kind = TY_UNION;
    if (tag)
      push_tag_scope(tag, ty);
  }

  // Apply early attributes
  if (early_attr.is_packed)
    ty->is_packed = true;
  if (early_attr.align)
    ty->align = early_attr.align;

  struct_members(&tok, tok, ty);
  union_layout(ty);
  tok = attribute_list(tok, ty, NULL);
  *rest = tok;
  return ty;
}

//
// Enum
//

static Type *enum_specifier(Token **rest, Token *tok) {
  Type *ty = enum_type();

  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  // C23/Objective-C fixed-size enum: enum Name : type { ... }
  if (equal(tok, ":")) {
    tok = tok->next;
    typename_(&tok, tok); // skip the underlying type
  }

  if (tag && !equal(tok, "{")) {
    Type *ty2 = find_tag(tag);
    if (!ty2)
      error_tok(tag, "unknown enum type");
    if (ty2->kind != TY_ENUM)
      error_tok(tag, "not an enum tag");
    *rest = tok;
    return ty2;
  }

  tok = skip(tok, "{");

  int val = 0;
  int i = 0;
  while (!consume(rest, tok, "}")) {
    if (i++ > 0)
      tok = skip(tok, ",");

    char *name = strndup_checked(tok->loc, tok->len);
    tok = tok->next;

    if (equal(tok, "=")) {
      val = const_expr_val(&tok, tok->next);
    }

    VarScope *vs = push_scope(name);
    vs->enum_ty = ty;
    vs->enum_val = val++;

    // Allow trailing comma
    if (equal(tok, ",") && equal(tok->next, "}")) {
      *rest = tok->next->next;
      break;
    }
  }

  if (tag)
    push_tag_scope(tag, ty);
  return ty;
}

//
// _Generic expression
//

static Node *generic_selection(Token **rest, Token *tok) {
  tok = skip(tok, "(");
  Node *ctrl = assign(&tok, tok);
  add_type(ctrl);
  tok = skip(tok, ",");

  Type *ctrl_ty = ctrl->ty;
  // Remove array/function decay for matching
  Node *ret = NULL;
  Node *default_node = NULL;

  while (!consume(rest, tok, ")")) {
    if (ret != NULL && default_node != NULL) {
      // Skip remaining associations
      while (!equal(tok, ")")) tok = tok->next;
      *rest = tok->next;
      break;
    }

    if (equal(tok, "default")) {
      tok = skip(tok->next, ":");
      default_node = assign(&tok, tok);
    } else {
      Type *ty = typename_(&tok, tok);
      tok = skip(tok, ":");
      Node *node = assign(&tok, tok);
      if (is_compatible(ctrl_ty, ty))
        ret = node;
    }

    if (!equal(tok, ")"))
      tok = skip(tok, ",");
  }

  if (!ret)
    ret = default_node;
  if (!ret)
    error_tok(ctrl->tok, "controlling expression type not compatible with any association");
  return ret;
}

//
// Expression parsing
//

// expr = assign ("," assign)*
static Node *expr(Token **rest, Token *tok) {
  Node *node = assign(&tok, tok);

  if (equal(tok, ","))
    return new_binary(ND_COMMA, node, expr(rest, tok->next), tok);

  *rest = tok;
  return node;
}

// Evaluate a constant expression
static int64_t const_expr_val(Token **rest, Token *tok) {
  Node *node = cond_expr(rest, tok);
  return eval(node);
}

static int64_t eval(Node *node) {
  return eval2(node, NULL);
}

static int64_t eval2(Node *node, char ***label) {
  add_type(node);

  if (is_flonum(node->ty))
    return (int64_t)eval_double(node);

  switch (node->kind) {
  case ND_ADD:
    return eval2(node->lhs, label) + eval(node->rhs);
  case ND_SUB:
    return eval2(node->lhs, label) - eval(node->rhs);
  case ND_MUL:
    return eval(node->lhs) * eval(node->rhs);
  case ND_DIV:
    if (node->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) / eval(node->rhs);
    return eval(node->lhs) / eval(node->rhs);
  case ND_MOD:
    if (node->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) % eval(node->rhs);
    return eval(node->lhs) % eval(node->rhs);
  case ND_NEG:
    return -eval(node->lhs);
  case ND_BITAND:
    return eval(node->lhs) & eval(node->rhs);
  case ND_BITOR:
    return eval(node->lhs) | eval(node->rhs);
  case ND_BITXOR:
    return eval(node->lhs) ^ eval(node->rhs);
  case ND_BITNOT:
    return ~eval(node->lhs);
  case ND_SHL:
    return eval(node->lhs) << eval(node->rhs);
  case ND_SHR:
    if (node->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) >> eval(node->rhs);
    return eval(node->lhs) >> eval(node->rhs);
  case ND_EQ:
    return eval(node->lhs) == eval(node->rhs);
  case ND_NE:
    return eval(node->lhs) != eval(node->rhs);
  case ND_LT:
    if (node->lhs->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) < (uint64_t)eval(node->rhs);
    return eval(node->lhs) < eval(node->rhs);
  case ND_LE:
    if (node->lhs->ty->is_unsigned)
      return (uint64_t)eval(node->lhs) <= (uint64_t)eval(node->rhs);
    return eval(node->lhs) <= eval(node->rhs);
  case ND_LOGAND:
    return eval(node->lhs) && eval(node->rhs);
  case ND_LOGOR:
    return eval(node->lhs) || eval(node->rhs);
  case ND_NOT:
    return !eval(node->lhs);
  case ND_COND:
    return eval(node->cond) ? eval2(node->then, label) : eval2(node->els, label);
  case ND_COMMA:
    return eval2(node->rhs, label);
  case ND_CAST: {
    int64_t val = eval2(node->lhs, label);
    if (is_integer(node->ty)) {
      switch (node->ty->size) {
      case 1: return node->ty->is_unsigned ? (uint8_t)val : (int8_t)val;
      case 2: return node->ty->is_unsigned ? (uint16_t)val : (int16_t)val;
      case 4: return node->ty->is_unsigned ? (uint32_t)val : (int32_t)val;
      }
    }
    return val;
  }
  case ND_NUM:
    return node->val;
  case ND_ADDR:
    return eval_rval(node->lhs, label);
  case ND_VAR:
    if (!label)
      error_tok(node->tok, "not a compile-time constant");
    if (node->var->ty->kind != TY_ARRAY && node->var->ty->kind != TY_FUNC)
      error_tok(node->tok, "not a compile-time constant");
    *label = &node->var->name;
    return 0;
  case ND_MEMBER:
    if (!label) {
      // Allow offsetof-like patterns: &((type*)0)->member
      // where the base is a pure numeric constant (no relocation needed)
      char **dummy_label = NULL;
      int64_t val = eval_rval(node->lhs, &dummy_label) + node->member->offset;
      if (dummy_label)
        error_tok(node->tok, "not a compile-time constant");
      return val;
    }
    return eval_rval(node->lhs, label) + node->member->offset;
  case ND_LABEL_VAL:
    if (!label)
      error_tok(node->tok, "not a compile-time constant");
    *label = &node->unique_label;
    return 0;
  default:
    error_tok(node->tok, "not a compile-time constant");
  }
}

static int64_t eval_rval(Node *node, char ***label) {
  switch (node->kind) {
  case ND_VAR:
    if (node->var->is_local)
      error_tok(node->tok, "not a compile-time constant");
    if (label) *label = &node->var->name;
    return 0;
  case ND_DEREF:
    return eval2(node->lhs, label);
  case ND_MEMBER:
    return eval_rval(node->lhs, label) + node->member->offset;
  case ND_CAST:
    return eval_rval(node->lhs, label);
  case ND_NUM:
    // For offsetof-like patterns: (type*)0 -> base address is a constant
    return node->val;
  default:
    error_tok(node->tok, "not a compile-time constant");
  }
}

// Evaluate a complex expression and return its real+imaginary parts.
// This handles the lowered form: (tmp.__real__ = R, tmp.__imag__ = I, tmp)
static void eval_complex(Node *node, double *re, double *im);

static double eval_double(Node *node) {
  add_type(node);

  if (is_integer(node->ty)) {
    if (node->ty->is_unsigned)
      return (unsigned long)eval(node);
    return eval(node);
  }

  // If the expression has complex type, extract the real part
  if (is_complex(node->ty)) {
    double re, im;
    eval_complex(node, &re, &im);
    return re;
  }

  switch (node->kind) {
  case ND_ADD:
    return eval_double(node->lhs) + eval_double(node->rhs);
  case ND_SUB:
    return eval_double(node->lhs) - eval_double(node->rhs);
  case ND_MUL:
    return eval_double(node->lhs) * eval_double(node->rhs);
  case ND_DIV:
    return eval_double(node->lhs) / eval_double(node->rhs);
  case ND_NEG:
    return -eval_double(node->lhs);
  case ND_COND:
    return eval_double(node->cond) ? eval_double(node->then) : eval_double(node->els);
  case ND_COMMA:
    return eval_double(node->rhs);
  case ND_CAST:
    if (is_flonum(node->lhs->ty))
      return eval_double(node->lhs);
    return eval(node->lhs);
  case ND_REAL:
    if (is_complex(node->lhs->ty)) {
      double re, im;
      eval_complex(node->lhs, &re, &im);
      return re;
    }
    return eval_double(node->lhs);
  case ND_IMAG:
    if (is_complex(node->lhs->ty)) {
      double re, im;
      eval_complex(node->lhs, &re, &im);
      return im;
    }
    return 0.0;
  case ND_NUM:
    return node->fval;
  default:
    error_tok(node->tok, "not a constant expression");
  }
}

// Evaluate a complex expression at compile time.
// The lowered form is: ((__real__ tmp = R, __imag__ tmp = I), tmp)
// We need to evaluate R and I.
static void eval_complex(Node *node, double *re, double *im) {
  add_type(node);

  if (node->kind == ND_COMMA) {
    // The rightmost child is the complex variable (just a temp).
    // The left side contains the assignments.
    // Pattern: COMMA(COMMA(ASSIGN(__real__ tmp, R), ASSIGN(__imag__ tmp, I)), tmp)
    if (node->lhs->kind == ND_COMMA) {
      Node *inner = node->lhs;
      // inner->lhs = ASSIGN(__real__ tmp, R)
      // inner->rhs = ASSIGN(__imag__ tmp, I)
      if (inner->lhs->kind == ND_ASSIGN && inner->rhs->kind == ND_ASSIGN) {
        *re = eval_double(inner->lhs->rhs);
        *im = eval_double(inner->rhs->rhs);
        return;
      }
    }
  }

  if (node->kind == ND_CAST) {
    // Cast to complex from scalar
    if (is_complex(node->ty) && !is_complex(node->lhs->ty)) {
      *re = eval_double(node->lhs);
      *im = 0.0;
      return;
    }
    eval_complex(node->lhs, re, im);
    return;
  }

  if (node->kind == ND_NUM) {
    *re = node->fval;
    *im = 0.0;
    return;
  }

  error_tok(node->tok, "not a compile-time constant");
}

// assign = cond (assign-op assign)?
// assign-op = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>="
static Node *assign(Token **rest, Token *tok) {
  Node *node = cond_expr(&tok, tok);

  if (equal(tok, "=")) {
    Node *rhs = assign(rest, tok->next);
    add_type(node);
    add_type(rhs);
    // For complex assignments, explicitly cast the rhs
    if (is_complex(node->ty) && rhs->ty && rhs->ty->kind != node->ty->kind)
      rhs = new_cast(rhs, node->ty);
    else if (is_complex(node->ty) && is_complex(rhs->ty) &&
             node->ty->base->kind != rhs->ty->base->kind)
      rhs = new_cast(rhs, node->ty);
    return new_binary(ND_ASSIGN, node, rhs, tok);
  }

  // Compound assignment operators.
  // For bitfield members, we can't use to_assign() because the pointer
  // indirection loses bitfield info. Instead: lhs = lhs op rhs directly.
  {
    add_type(node);
    bool is_bf = (node->kind == ND_MEMBER && node->member && node->member->is_bitfield);
    NodeKind op = 0;
    bool is_add_sub = false;

    if (equal(tok, "+="))      { op = ND_ADD; is_add_sub = true; }
    else if (equal(tok, "-=")) { op = ND_SUB; is_add_sub = true; }
    else if (equal(tok, "*=")) { op = ND_MUL; }
    else if (equal(tok, "/=")) { op = ND_DIV; }
    else if (equal(tok, "%=")) { op = ND_MOD; }
    else if (equal(tok, "&=")) { op = ND_BITAND; }
    else if (equal(tok, "|=")) { op = ND_BITOR; }
    else if (equal(tok, "^=")) { op = ND_BITXOR; }
    else if (equal(tok, "<<=")){ op = ND_SHL; }
    else if (equal(tok, ">>=")){op = ND_SHR; }

    if (op) {
      Token *optok = tok;
      Node *rhs = assign(rest, tok->next);

      if (is_bf) {
        // Bitfield: lhs = lhs op rhs (no pointer indirection)
        Node *val;
        if (is_add_sub && op == ND_ADD)
          val = new_add(node, rhs, optok);
        else if (is_add_sub && op == ND_SUB)
          val = new_sub(node, rhs, optok);
        else
          val = new_binary(op, node, rhs, optok);
        return new_binary(ND_ASSIGN, node, val, optok);
      }

      // Complex compound assignment: x op= y → x = x op y
      if (is_complex(node->ty)) {
        Node *val;
        if (op == ND_ADD)
          val = new_add(node, rhs, optok);
        else if (op == ND_SUB)
          val = new_sub(node, rhs, optok);
        else if (op == ND_MUL)
          val = new_complex_mul(node, rhs, optok);
        else if (op == ND_DIV)
          val = new_complex_div(node, rhs, optok);
        else
          error_tok(optok, "invalid complex compound assignment");
        return new_binary(ND_ASSIGN, node, val, optok);
      }

      // Normal: use to_assign with temp pointer
      Node *binary;
      if (is_add_sub && op == ND_ADD)
        binary = new_add(node, rhs, optok);
      else if (is_add_sub && op == ND_SUB)
        binary = new_sub(node, rhs, optok);
      else
        binary = new_binary(op, node, rhs, optok);
      return to_assign(binary);
    }
  }

  *rest = tok;
  return node;
}

// Helpers for compound assignment: a op= b → tmp=&a, *tmp = *tmp op b
static Node *to_assign(Node *binary) {
  add_type(binary->lhs);
  add_type(binary->rhs);
  Token *tok = binary->tok;

  Obj *var = new_lvar("", pointer_to(binary->lhs->ty));
  Node *expr1 = new_binary(ND_ASSIGN, new_var_node(var, tok),
                            new_unary(ND_ADDR, binary->lhs, tok), tok);
  Node *expr2 = new_binary(ND_ASSIGN,
                            new_unary(ND_DEREF, new_var_node(var, tok), tok),
                            new_binary(binary->kind,
                                       new_unary(ND_DEREF, new_var_node(var, tok), tok),
                                       binary->rhs, tok),
                            tok);
  return new_binary(ND_COMMA, expr1, expr2, tok);
}

// For pointer arithmetic: ptr + int → ptr + int * sizeof(*ptr)
static Node *new_add(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  // complex + complex (or complex + scalar, scalar + complex)
  if (is_complex(lhs->ty) || is_complex(rhs->ty)) {
    // Promote scalar to complex if needed
    Type *cty;
    if (is_complex(lhs->ty) && is_complex(rhs->ty)) {
      // Both complex: use wider base
      Type *lb = lhs->ty->base, *rb = rhs->ty->base;
      Type *base = (lb->size >= rb->size) ? lb : rb;
      if (lb->kind == TY_DOUBLE || rb->kind == TY_DOUBLE) base = ty_double;
      if (lb->kind == TY_LDOUBLE || rb->kind == TY_LDOUBLE) base = ty_ldouble;
      cty = complex_type(base);
    } else if (is_complex(lhs->ty)) {
      cty = lhs->ty;
    } else {
      cty = rhs->ty;
    }
    Node *lr = is_complex(lhs->ty) ? new_real(lhs, tok) : lhs;
    Node *li = is_complex(lhs->ty) ? new_imag(lhs, tok) : new_num(0, tok);
    Node *rr = is_complex(rhs->ty) ? new_real(rhs, tok) : rhs;
    Node *ri = is_complex(rhs->ty) ? new_imag(rhs, tok) : new_num(0, tok);
    return new_complex_val(
      new_binary(ND_ADD, lr, rr, tok),
      new_binary(ND_ADD, li, ri, tok),
      cty, tok);
  }

  // num + num
  if (is_numeric(lhs->ty) && is_numeric(rhs->ty))
    return new_binary(ND_ADD, lhs, rhs, tok);

  // ptr + ptr = error
  if (lhs->ty->base && rhs->ty->base)
    error_tok(tok, "invalid operands");

  // Canonicalize: num + ptr → ptr + num
  if (!lhs->ty->base && rhs->ty->base) {
    Node *tmp = lhs;
    lhs = rhs;
    rhs = tmp;
  }

  // ptr + num
  // For VLA base types, use the runtime-computed size
  Node *elem_size;
  if (lhs->ty->base->vla_size)
    elem_size = new_var_node(lhs->ty->base->vla_size, tok);
  else
    elem_size = new_long(lhs->ty->base->size, tok);
  rhs = new_binary(ND_MUL, rhs, elem_size, tok);
  return new_binary(ND_ADD, lhs, rhs, tok);
}

static Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  // complex - complex
  if (is_complex(lhs->ty) || is_complex(rhs->ty)) {
    Type *cty = is_complex(lhs->ty) ? lhs->ty : rhs->ty;
    Node *lr = is_complex(lhs->ty) ? new_real(lhs, tok) : lhs;
    Node *li = is_complex(lhs->ty) ? new_imag(lhs, tok) : new_num(0, tok);
    Node *rr = is_complex(rhs->ty) ? new_real(rhs, tok) : rhs;
    Node *ri = is_complex(rhs->ty) ? new_imag(rhs, tok) : new_num(0, tok);
    return new_complex_val(
      new_binary(ND_SUB, lr, rr, tok),
      new_binary(ND_SUB, li, ri, tok),
      cty, tok);
  }

  // num - num
  if (is_numeric(lhs->ty) && is_numeric(rhs->ty))
    return new_binary(ND_SUB, lhs, rhs, tok);

  // ptr - num
  if (lhs->ty->base && is_integer(rhs->ty)) {
    Node *elem_size;
    if (lhs->ty->base->vla_size)
      elem_size = new_var_node(lhs->ty->base->vla_size, tok);
    else
      elem_size = new_long(lhs->ty->base->size, tok);
    rhs = new_binary(ND_MUL, rhs, elem_size, tok);
    add_type(rhs);
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = lhs->ty;
    return node;
  }

  // ptr - ptr = difference in elements
  if (lhs->ty->base && rhs->ty->base) {
    Node *node = new_binary(ND_SUB, lhs, rhs, tok);
    node->ty = ty_long;
    Node *divisor;
    if (lhs->ty->base->vla_size)
      divisor = new_var_node(lhs->ty->base->vla_size, tok);
    else
      divisor = new_num(lhs->ty->base->size, tok);
    return new_binary(ND_DIV, node, divisor, tok);
  }

  error_tok(tok, "invalid operands");
}

// cond = logor ("?" expr ":" cond)?
static Node *cond_expr(Token **rest, Token *tok) {
  Node *node = logor(&tok, tok);

  if (!equal(tok, "?")) {
    *rest = tok;
    return node;
  }

  Node *cond = new_node(ND_COND, tok);
  cond->cond = node;
  tok = tok->next;
  cond->then = expr(&tok, tok);
  tok = skip(tok, ":");
  cond->els = cond_expr(rest, tok);
  return cond;
}

static Node *logor(Token **rest, Token *tok) {
  Node *node = logand(&tok, tok);
  while (equal(tok, "||")) {
    Token *start = tok;
    node = new_binary(ND_LOGOR, node, logand(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

static Node *logand(Token **rest, Token *tok) {
  Node *node = bitor_(&tok, tok);
  while (equal(tok, "&&")) {
    Token *start = tok;
    node = new_binary(ND_LOGAND, node, bitor_(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

static Node *bitor_(Token **rest, Token *tok) {
  Node *node = bitxor(&tok, tok);
  while (equal(tok, "|")) {
    Token *start = tok;
    node = new_binary(ND_BITOR, node, bitxor(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

static Node *bitxor(Token **rest, Token *tok) {
  Node *node = bitand(&tok, tok);
  while (equal(tok, "^")) {
    Token *start = tok;
    node = new_binary(ND_BITXOR, node, bitand(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

static Node *bitand(Token **rest, Token *tok) {
  Node *node = equality(&tok, tok);
  while (equal(tok, "&")) {
    Token *start = tok;
    node = new_binary(ND_BITAND, node, equality(&tok, tok->next), start);
  }
  *rest = tok;
  return node;
}

static Node *equality(Token **rest, Token *tok) {
  Node *node = relational(&tok, tok);
  for (;;) {
    Token *start = tok;
    if (equal(tok, "==")) {
      Node *rhs = relational(&tok, tok->next);
      add_type(node);
      add_type(rhs);
      if (is_complex(node->ty) || is_complex(rhs->ty)) {
        // Complex ==: both real and imag must be equal
        Node *lr = is_complex(node->ty) ? new_real(node, start) : node;
        Node *li = is_complex(node->ty) ? new_imag(node, start) : new_num(0, start);
        Node *rr = is_complex(rhs->ty) ? new_real(rhs, start) : rhs;
        Node *ri = is_complex(rhs->ty) ? new_imag(rhs, start) : new_num(0, start);
        node = new_binary(ND_LOGAND,
          new_binary(ND_EQ, lr, rr, start),
          new_binary(ND_EQ, li, ri, start), start);
      } else {
        node = new_binary(ND_EQ, node, rhs, start);
      }
      continue;
    }
    if (equal(tok, "!=")) {
      Node *rhs = relational(&tok, tok->next);
      add_type(node);
      add_type(rhs);
      if (is_complex(node->ty) || is_complex(rhs->ty)) {
        // Complex !=: either real or imag differs
        Node *lr = is_complex(node->ty) ? new_real(node, start) : node;
        Node *li = is_complex(node->ty) ? new_imag(node, start) : new_num(0, start);
        Node *rr = is_complex(rhs->ty) ? new_real(rhs, start) : rhs;
        Node *ri = is_complex(rhs->ty) ? new_imag(rhs, start) : new_num(0, start);
        node = new_binary(ND_LOGOR,
          new_binary(ND_NE, lr, rr, start),
          new_binary(ND_NE, li, ri, start), start);
      } else {
        node = new_binary(ND_NE, node, rhs, start);
      }
      continue;
    }
    *rest = tok;
    return node;
  }
}

static Node *relational(Token **rest, Token *tok) {
  Node *node = shift(&tok, tok);
  for (;;) {
    Token *start = tok;
    if (equal(tok, "<")) {
      node = new_binary(ND_LT, node, shift(&tok, tok->next), start);
      continue;
    }
    if (equal(tok, "<=")) {
      node = new_binary(ND_LE, node, shift(&tok, tok->next), start);
      continue;
    }
    if (equal(tok, ">")) {
      node = new_binary(ND_LT, shift(&tok, tok->next), node, start);
      continue;
    }
    if (equal(tok, ">=")) {
      node = new_binary(ND_LE, shift(&tok, tok->next), node, start);
      continue;
    }
    *rest = tok;
    return node;
  }
}

static Node *shift(Token **rest, Token *tok) {
  Node *node = add(&tok, tok);
  for (;;) {
    Token *start = tok;
    if (equal(tok, "<<")) {
      node = new_binary(ND_SHL, node, add(&tok, tok->next), start);
      continue;
    }
    if (equal(tok, ">>")) {
      node = new_binary(ND_SHR, node, add(&tok, tok->next), start);
      continue;
    }
    *rest = tok;
    return node;
  }
}

// add = mul ("+" mul | "-" mul)*
static Node *add(Token **rest, Token *tok) {
  Node *node = mul(&tok, tok);
  for (;;) {
    Token *start = tok;
    if (equal(tok, "+")) {
      node = new_add(node, mul(&tok, tok->next), start);
      continue;
    }
    if (equal(tok, "-")) {
      node = new_sub(node, mul(&tok, tok->next), start);
      continue;
    }
    *rest = tok;
    return node;
  }
}

// mul = cast ("*" cast | "/" cast | "%" cast)*
// Complex multiplication: (a+bi)(c+di) = (ac-bd) + (ad+bc)i
static Node *new_complex_mul(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);
  Type *cty = is_complex(lhs->ty) ? lhs->ty : rhs->ty;
  Node *a = is_complex(lhs->ty) ? new_real(lhs, tok) : lhs;
  Node *b = is_complex(lhs->ty) ? new_imag(lhs, tok) : new_num(0, tok);
  Node *c = is_complex(rhs->ty) ? new_real(rhs, tok) : rhs;
  Node *d = is_complex(rhs->ty) ? new_imag(rhs, tok) : new_num(0, tok);
  Node *real = new_binary(ND_SUB,
    new_binary(ND_MUL, a, c, tok),
    new_binary(ND_MUL, b, d, tok), tok);
  Node *imag = new_binary(ND_ADD,
    new_binary(ND_MUL, a, d, tok),
    new_binary(ND_MUL, b, c, tok), tok);
  return new_complex_val(real, imag, cty, tok);
}

// Complex division: (a+bi)/(c+di) = ((ac+bd) + (bc-ad)i) / (c^2+d^2)
static Node *new_complex_div(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);
  Type *cty = is_complex(lhs->ty) ? lhs->ty : rhs->ty;
  Node *a = is_complex(lhs->ty) ? new_real(lhs, tok) : lhs;
  Node *b = is_complex(lhs->ty) ? new_imag(lhs, tok) : new_num(0, tok);
  Node *c = is_complex(rhs->ty) ? new_real(rhs, tok) : rhs;
  Node *d = is_complex(rhs->ty) ? new_imag(rhs, tok) : new_num(0, tok);
  Node *denom = new_binary(ND_ADD,
    new_binary(ND_MUL, c, c, tok),
    new_binary(ND_MUL, d, d, tok), tok);
  Node *real = new_binary(ND_DIV,
    new_binary(ND_ADD,
      new_binary(ND_MUL, a, c, tok),
      new_binary(ND_MUL, b, d, tok), tok),
    denom, tok);
  Node *imag = new_binary(ND_DIV,
    new_binary(ND_SUB,
      new_binary(ND_MUL, b, c, tok),
      new_binary(ND_MUL, a, d, tok), tok),
    denom, tok);
  return new_complex_val(real, imag, cty, tok);
}

static Node *mul(Token **rest, Token *tok) {
  Node *node = cast(&tok, tok);
  for (;;) {
    Token *start = tok;
    if (equal(tok, "*")) {
      Node *rhs = cast(&tok, tok->next);
      add_type(node);
      add_type(rhs);
      if (is_complex(node->ty) || is_complex(rhs->ty))
        node = new_complex_mul(node, rhs, start);
      else
        node = new_binary(ND_MUL, node, rhs, start);
      continue;
    }
    if (equal(tok, "/")) {
      Node *rhs = cast(&tok, tok->next);
      add_type(node);
      add_type(rhs);
      if (is_complex(node->ty) || is_complex(rhs->ty))
        node = new_complex_div(node, rhs, start);
      else
        node = new_binary(ND_DIV, node, rhs, start);
      continue;
    }
    if (equal(tok, "%")) {
      node = new_binary(ND_MOD, node, cast(&tok, tok->next), start);
      continue;
    }
    *rest = tok;
    return node;
  }
}

// cast = "(" type-name ")" cast | unary
static Node *cast(Token **rest, Token *tok) {
  if (equal(tok, "(") && is_typename(tok->next)) {
    Token *start = tok;
    Type *ty = typename_(&tok, tok->next);
    tok = skip(tok, ")");

    // Compound literal (not valid for void type)
    if (equal(tok, "{") && ty->kind != TY_VOID)
      return unary(rest, start);

    // Cast (including cast to void)
    Node *node = new_cast(cast(rest, tok), ty);
    node->tok = start;
    return node;
  }

  return unary(rest, tok);
}

// unary = ("+" | "-" | "&" | "*" | "!" | "~") cast
//       | ("++" | "--") unary
//       | "sizeof" unary | "sizeof" "(" type-name ")"
//       | "_Alignof" "(" type-name ")"
//       | "&&" ident (label address, GCC extension)
//       | postfix
static Node *unary(Token **rest, Token *tok) {
  if (equal(tok, "+"))
    return cast(rest, tok->next);

  if (equal(tok, "-")) {
    Node *operand = cast(rest, tok->next);
    add_type(operand);
    if (is_complex(operand->ty)) {
      return new_complex_val(
        new_unary(ND_NEG, new_real(operand, tok), tok),
        new_unary(ND_NEG, new_imag(operand, tok), tok),
        operand->ty, tok);
    }
    return new_unary(ND_NEG, operand, tok);
  }

  if (equal(tok, "&"))
    return new_unary(ND_ADDR, cast(rest, tok->next), tok);

  if (equal(tok, "*")) {
    Node *node = new_unary(ND_DEREF, cast(rest, tok->next), tok);
    add_type(node);
    return node;
  }

  if (equal(tok, "!"))
    return new_unary(ND_NOT, cast(rest, tok->next), tok);

  if (equal(tok, "~")) {
    Node *operand = cast(rest, tok->next);
    add_type(operand);
    if (is_complex(operand->ty)) {
      // Complex conjugate: negate the imaginary part
      return new_complex_val(
        new_real(operand, tok),
        new_unary(ND_NEG, new_imag(operand, tok), tok),
        operand->ty, tok);
    }
    return new_unary(ND_BITNOT, operand, tok);
  }

  // __real__ and __imag__ (GCC extensions)
  if (equal(tok, "__real__")) {
    Node *node = new_unary(ND_REAL, cast(rest, tok->next), tok);
    add_type(node);
    return node;
  }
  if (equal(tok, "__imag__")) {
    Node *node = new_unary(ND_IMAG, cast(rest, tok->next), tok);
    add_type(node);
    return node;
  }

  // Pre-increment: ++i → i += 1
  if (equal(tok, "++")) {
    Node *operand = unary(rest, tok->next);
    add_type(operand);
    if (operand->kind == ND_MEMBER && operand->member && operand->member->is_bitfield) {
      // Bitfield: lhs = lhs + 1 (no pointer indirection)
      return new_binary(ND_ASSIGN, operand, new_add(operand, new_num(1, tok), tok), tok);
    }
    return to_assign(new_add(operand, new_num(1, tok), tok));
  }

  // Pre-decrement: --i → i -= 1
  if (equal(tok, "--")) {
    Node *operand = unary(rest, tok->next);
    add_type(operand);
    if (operand->kind == ND_MEMBER && operand->member && operand->member->is_bitfield) {
      // Bitfield: lhs = lhs - 1 (no pointer indirection)
      return new_binary(ND_ASSIGN, operand, new_sub(operand, new_num(1, tok), tok), tok);
    }
    return to_assign(new_sub(operand, new_num(1, tok), tok));
  }

  // &&label (GNU extension, for computed goto)
  if (equal(tok, "&&")) {
    Node *node = new_node(ND_LABEL_VAL, tok);
    node->label = strndup_checked(tok->next->loc, tok->next->len);
    // Check if we already have a unique_label for this label name
    // (can happen when initializer is parsed twice, e.g., for flexible arrays).
    // All ND_LABEL_VAL nodes for the same label must share the same unique_label.
    char *shared_label = NULL;
    for (Node *g = gotos; g; g = g->goto_next) {
      if (g->kind == ND_LABEL_VAL && !strcmp(g->label, node->label)) {
        shared_label = g->unique_label;
        break;
      }
    }
    // Use a non-temporary symbol name (no ".L" prefix) so it survives
    // Mach-O linking when referenced from data sections.
    node->unique_label = shared_label ? shared_label : format("L_cg_%d", label_cnt++);
    node->goto_next = gotos;
    gotos = node;
    *rest = tok->next->next;
    return node;
  }

  // sizeof
  if (equal(tok, "sizeof")) {
    if (equal(tok->next, "(") && is_typename(tok->next->next)) {
      Type *ty = typename_(&tok, tok->next->next);
      *rest = skip(tok, ")");
      // VLA or struct/union with VLA members: sizeof is computed at runtime
      if (ty->vla_size)
        return new_var_node(ty->vla_size, tok);
      if (ty->kind == TY_VLA) {
        // Fallback: should not happen if compute_vla_size was called
      }
      return new_ulong(ty->size, tok);
    }
    Node *node = unary(&tok, tok->next);
    add_type(node);
    // For VLA variables, check the original type (before VLA-to-pointer decay)
    if (node->kind == ND_VAR && node->var->ty->vla_size) {
      *rest = tok;
      return new_var_node(node->var->ty->vla_size, tok);
    }
    if (node->ty->vla_size) {
      *rest = tok;
      return new_var_node(node->ty->vla_size, tok);
    }
    *rest = tok;
    return new_ulong(node->ty->size, tok);
  }

  // _Alignof
  if (equal(tok, "_Alignof") || equal(tok, "__alignof__")) {
    tok = skip(tok->next, "(");
    if (is_typename(tok)) {
      Type *ty = typename_(&tok, tok);
      *rest = skip(tok, ")");
      return new_ulong(ty->align, tok);
    }
    Node *node = unary(&tok, tok);
    add_type(node);
    *rest = skip(tok, ")");
    return new_ulong(node->ty->align, tok);
  }

  return postfix(rest, tok);
}

// Find struct member by name
__attribute__((unused))
static Member *get_struct_member(Type *ty, Token *tok) {
  for (Member *mem = ty->members; mem; mem = mem->next) {
    // Named member
    if (mem->name && mem->name->len == tok->len &&
        !strncmp(mem->name->loc, tok->loc, tok->len))
      return mem;

    // Anonymous struct/union member: recurse
    if (!mem->name && (mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION)) {
      Member *m = get_struct_member(mem->ty, tok);
      if (m)
        return m;
    }
  }
  return NULL;
}

// Build a chain of member accesses for accessing nested anonymous struct members
static Node *struct_ref(Node *node, Token *tok) {
  add_type(node);
  Type *ty = node->ty;

  // Follow origin chain to find the completed type with members
  while (ty->origin && !ty->members &&
         (ty->kind == TY_STRUCT || ty->kind == TY_UNION))
    ty = ty->origin;

  if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
    error_tok(node->tok, "not a struct/union");

  // First try direct member
  for (Member *mem = ty->members; mem; mem = mem->next) {
    if (mem->name && mem->name->len == tok->len &&
        !strncmp(mem->name->loc, tok->loc, tok->len)) {
      Node *n = new_unary(ND_MEMBER, node, tok);
      n->member = mem;
      return n;
    }
  }

  // Try anonymous struct/union members
  for (Member *mem = ty->members; mem; mem = mem->next) {
    if (!mem->name && (mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION)) {
      Node *n = new_unary(ND_MEMBER, node, tok);
      n->member = mem;
      Node *result = struct_ref(n, tok);
      if (result)
        return result;
    }
  }

  error_tok(tok, "no such member");
}

// postfix = "(" type-name ")" "{" initializer-list "}" (compound literal)
//         | primary ("[" expr "]" | "." ident | "->" ident | "++" | "--")*
static Node *postfix(Token **rest, Token *tok) {
  // Compound literal
  if (equal(tok, "(") && is_typename(tok->next)) {
    Token *start = tok;
    Type *ty = typename_(&tok, tok->next);
    tok = skip(tok, ")");

    if (equal(tok, "{")) {
      // Compound literal
      if (scope->next) {
        // Local scope: create a temp, init it, return its address
        Obj *var = new_lvar("", ty);
        Node *init = lvar_initializer(rest, tok, var);
        // The compound literal evaluates to the variable itself
        Node *ref = new_var_node(var, start);
        return new_binary(ND_COMMA, init, ref, start);
      }

      Obj *var = new_anon_gvar(ty);
      gvar_initializer(rest, tok, var);
      return new_var_node(var, start);
    }

    // It was a cast, not compound literal
    // Reparse
    *rest = start;
    return cast(rest, start);
  }

  Node *node = primary(&tok, tok);

  for (;;) {
    // Array access: a[b] → *(a + b)
    if (equal(tok, "[")) {
      Token *start = tok;
      Node *idx = expr(&tok, tok->next);
      tok = skip(tok, "]");
      node = new_unary(ND_DEREF, new_add(node, idx, start), start);
      continue;
    }

    // Struct member access: a.b
    if (equal(tok, ".")) {
      node = struct_ref(node, tok->next);
      tok = tok->next->next;
      continue;
    }

    // Struct member access via pointer: a->b → (*a).b
    if (equal(tok, "->")) {
      node = new_unary(ND_DEREF, node, tok);
      node = struct_ref(node, tok->next);
      tok = tok->next->next;
      continue;
    }

    // Function call through expression: expr(args)
    if (equal(tok, "(")) {
      add_type(node);
      // node should be a function or function pointer
      Type *func_ty = NULL;
      if (node->ty->kind == TY_FUNC)
        func_ty = node->ty;
      else if (node->ty->kind == TY_PTR && node->ty->base->kind == TY_FUNC)
        func_ty = node->ty->base;

      if (func_ty) {
        Token *start = tok;
        tok = tok->next; // skip (
        Node head = {};
        Node *cur = &head;
        Type *param_ty = func_ty->params;

        while (!equal(tok, ")")) {
          if (cur != &head)
            tok = skip(tok, ",");
          Node *arg = assign(&tok, tok);
          add_type(arg);
          if (param_ty) {
            if (param_ty->kind != TY_STRUCT && param_ty->kind != TY_UNION)
              arg = new_cast(arg, param_ty);
            param_ty = param_ty->next;
          } else if (arg->ty->kind == TY_FLOAT) {
            arg = new_cast(arg, ty_double);
          }
          cur = cur->next = arg;
        }
        tok = skip(tok, ")");

        Node *call = new_node(ND_FUNCALL, start);
        call->funcname = node->ty->kind == TY_FUNC && node->var ?
                         node->var->name : "__indirect_call";
        call->func_ty = func_ty;
        call->ty = func_ty->return_ty;
        call->args = head.next;
        call->lhs = node; // the expression to call through
        node = call;
        continue;
      }
    }

    // Post-increment: i++ → (typeof i)((tmp = &i, old = *tmp, *tmp = *tmp + 1, old))
    if (equal(tok, "++")) {
      node = new_inc_dec(node, tok, 1);
      tok = tok->next;
      continue;
    }

    if (equal(tok, "--")) {
      node = new_inc_dec(node, tok, -1);
      tok = tok->next;
      continue;
    }

    *rest = tok;
    return node;
  }
}

static Node *new_inc_dec(Node *node, Token *tok, int addend) {
  add_type(node);

  // Bitfield: avoid pointer indirection which loses bitfield metadata.
  // Generate: (old = bf, bf = bf + addend, old)
  if (node->kind == ND_MEMBER && node->member && node->member->is_bitfield) {
    Node *old_val = new_var_node(new_lvar("", node->ty), tok);
    Node *save_old = new_binary(ND_ASSIGN, old_val, node, tok);
    Node *inc = new_binary(ND_ASSIGN, node,
      new_add(node, new_num(addend, tok), tok), tok);
    return new_binary(ND_COMMA, save_old,
      new_binary(ND_COMMA, inc,
        new_var_node(old_val->var, tok), tok), tok);
  }

  Node *tmp = new_binary(ND_ASSIGN,
    new_var_node(new_lvar("", pointer_to(node->ty)), tok),
    new_unary(ND_ADDR, node, tok), tok);

  Node *use = new_unary(ND_DEREF, new_var_node(tmp->lhs->var, tok), tok);
  Node *inc = new_binary(ND_ASSIGN,
    new_unary(ND_DEREF, new_var_node(tmp->lhs->var, tok), tok),
    new_add(new_unary(ND_DEREF, new_var_node(tmp->lhs->var, tok), tok),
            new_num(addend, tok), tok), tok);

  // (tmp = &x, use = *tmp, *tmp = *tmp + 1, use - 1 + 1)  or similar
  // Simpler approach: { tmp=&x; val=*tmp; *tmp = *tmp + addend; val }
  Node *old_val = new_var_node(new_lvar("", node->ty), tok);
  Node *save_old = new_binary(ND_ASSIGN, old_val, use, tok);

  // We need: (tmp=&x, old=*tmp, *tmp=*tmp+addend, old)
  return new_binary(ND_COMMA, tmp,
    new_binary(ND_COMMA, save_old,
      new_binary(ND_COMMA, inc,
        new_var_node(old_val->var, tok), tok), tok), tok);
}

// primary = "(" "{" stmt-expr "}" ")"    (GNU extension: statement expression)
//         | "(" expr ")"
//         | ident args?
//         | str
//         | num
static Node *primary(Token **rest, Token *tok) {
  Token *start = tok;

  // __builtin_types_compatible_p(type1, type2) — GCC extension
  if (equal(tok, "__builtin_types_compatible_p")) {
    tok = skip(tok->next, "(");
    Type *t1 = typename_(&tok, tok);
    tok = skip(tok, ",");
    Type *t2 = typename_(&tok, tok);
    *rest = skip(tok, ")");

    // Compare types for GCC __builtin_types_compatible_p semantics:
    // - Top-level qualifiers (const, volatile) are ignored
    // - Qualifiers on pointed-to types DO matter
    // - Enums are compatible with int but not with other enum types
    int result = 0;
    {
      Type *a = t1, *b = t2;

      // Note: enum types are NOT resolved to int here.
      // GCC treats different enum types as incompatible.
      // Enum constants (typeof(enum_val)) already have type int from the parser.

      if (a->kind != b->kind) {
        result = 0;
      } else if (a->kind == TY_ENUM) {
        // Two enum types: compatible only if they are the same enum.
        // Follow origin chain to find the original type allocation.
        Type *oa = a, *ob = b;
        while (oa->origin) oa = oa->origin;
        while (ob->origin) ob = ob->origin;
        result = (oa == ob);
      } else if (a->kind == TY_PTR) {
        // Pointers: pointed-to types must be fully compatible INCLUDING qualifiers
        Type *pa = a->base, *pb = b->base;
        if (pa->kind != pb->kind)
          result = 0;
        else if (pa->is_const != pb->is_const || pa->is_volatile != pb->is_volatile)
          result = 0;
        else if (pa->is_unsigned != pb->is_unsigned)
          result = 0;
        else if (pa->kind == TY_PTR) {
          // Pointer to pointer: recursively compare
          result = (pa->base && pb->base &&
                    pa->base->kind == pb->base->kind &&
                    pa->base->is_unsigned == pb->base->is_unsigned &&
                    pa->base->size == pb->base->size);
        } else if (pa->kind == TY_STRUCT || pa->kind == TY_UNION)
          result = (pa->size == pb->size && pa->members == pb->members);
        else
          result = (pa->size == pb->size);
      } else if (a->kind == TY_ARRAY) {
        // Arrays: element types must be compatible; sizes must match unless one is []
        Type *ea = a->base, *eb = b->base;
        if (ea->kind == TY_ENUM) ea = ty_int;
        if (eb->kind == TY_ENUM) eb = ty_int;
        if (ea->kind != eb->kind || ea->is_unsigned != eb->is_unsigned || ea->size != eb->size)
          result = 0;
        else if (a->array_len >= 0 && b->array_len >= 0 && a->array_len != b->array_len)
          result = 0;
        else
          result = 1;
      } else if (a->kind == TY_STRUCT || a->kind == TY_UNION) {
        result = (a->size == b->size && a->members == b->members);
      } else if (a->kind == TY_FUNC) {
        result = 0;
      } else {
        // Scalar types: kind and signedness must match; top-level qualifiers ignored
        result = (a->is_unsigned == b->is_unsigned && a->size == b->size);
      }
    }
    return new_num(result, start);
  }

  // __builtin_va_arg(ap, type)
  if (equal(tok, "__builtin_va_arg")) {
    tok = skip(tok->next, "(");
    Node *ap = assign(&tok, tok);
    tok = skip(tok, ",");
    Type *ty = typename_(&tok, tok);
    *rest = skip(tok, ")");

    // Apple ARM64 va_arg: read value from current ap position, then advance ap.
    // tmp = (type*)ap; ap = (char*)ap + align(sizeof(type), 8); return *tmp
    add_type(ap);
    int sz = align_to(ty->size < 8 ? 8 : ty->size, 8);

    // Create: *((type*)(ap_old)), where ap_old = ap before increment
    // And ap += size as side effect
    // Build: (tmp = ap, ap = (void*)((char*)ap + sz), *(type*)tmp)
    Obj *tmp = new_lvar("", pointer_to(ty));
    // tmp = (type*)ap
    Node *save = new_binary(ND_ASSIGN, new_var_node(tmp, tok),
                            new_cast(ap, pointer_to(ty)), tok);
    // ap = (void*)((char*)ap + sz)
    // We need the lvalue of ap — use the same expression
    // For simplicity, increment ap as: ap = ap + sz (pointer arithmetic on void*)
    // Since ap is void*, we cast to char* for arithmetic
    Node *ap_as_char = new_cast(ap, pointer_to(ty_char));
    Node *advance = new_binary(ND_ASSIGN, ap,
      new_cast(new_binary(ND_ADD, ap_as_char, new_num(sz, tok), tok),
               pointer_to(ty_void)), tok);

    // Result: *tmp
    Node *result = new_unary(ND_DEREF, new_var_node(tmp, tok), tok);

    // Chain: (save, advance, result)
    return new_binary(ND_COMMA, save,
             new_binary(ND_COMMA, advance, result, tok), tok);
  }

  // __builtin_va_start(ap, last_named) — set ap to point to variadic args
  if (equal(tok, "__builtin_va_start")) {
    tok = skip(tok->next, "(");
    Node *ap = assign(&tok, tok);
    tok = skip(tok, ",");
    // Skip the last_named parameter — we don't need it on ARM64
    assign(&tok, tok);
    *rest = skip(tok, ")");

    // On Apple ARM64, variadic args are on the stack after named args.
    // The function prologue created __va_area__ for this.
    // ap = __va_area__
    if (current_fn && current_fn->va_area) {
      // ap = __va_area__ (which holds the pointer to the variadic args on stack)
      return new_binary(ND_ASSIGN, ap,
               new_cast(new_var_node(current_fn->va_area, tok),
                        pointer_to(ty_void)), tok);
    }
    return new_num(0, tok);
  }

  // __builtin_va_end — no-op
  if (equal(tok, "__builtin_va_end")) {
    tok = skip(tok->next, "(");
    assign(&tok, tok);
    *rest = skip(tok, ")");
    return new_num(0, tok);
  }

  // __builtin_va_copy(dest, src) — dest = src
  if (equal(tok, "__builtin_va_copy")) {
    tok = skip(tok->next, "(");
    Node *dest = assign(&tok, tok);
    tok = skip(tok, ",");
    Node *src = assign(&tok, tok);
    *rest = skip(tok, ")");
    return new_binary(ND_ASSIGN, dest, src, tok);
  }

  // __builtin_prefetch(addr, ...) — no-op, evaluate and discard args
  if (equal(tok, "__builtin_prefetch")) {
    tok = skip(tok->next, "(");
    // Parse and discard all arguments
    Node *node = assign(&tok, tok);
    while (consume(&tok, tok, ","))
      assign(&tok, tok);
    *rest = skip(tok, ")");
    // Return 0 but evaluate the first arg for side effects via comma
    return new_binary(ND_COMMA, node, new_num(0, tok), tok);
  }

  // __builtin_alloca(size) — stack allocation
  if (equal(tok, "__builtin_alloca") || equal(tok, "alloca")) {
    tok = skip(tok->next, "(");
    Node *size = assign(&tok, tok);
    *rest = skip(tok, ")");
    Node *node = new_unary(ND_BUILTIN_ALLOCA, size, tok);
    node->ty = pointer_to(ty_void);
    return node;
  }

  // __builtin_clz[l|ll](x) — count leading zeros
  if (equal(tok, "__builtin_clz") || equal(tok, "__builtin_clzl") ||
      equal(tok, "__builtin_clzll")) {
    bool is64 = !equal(tok, "__builtin_clz");
    tok = skip(tok->next, "(");
    Node *arg = assign(&tok, tok);
    if (is64) arg = new_cast(arg, ty_long);
    *rest = skip(tok, ")");
    Node *node = new_unary(ND_BUILTIN_CLZ, arg, tok);
    node->ty = ty_int;
    node->val = is64;
    return node;
  }

  // __builtin_ctz[l|ll](x) — count trailing zeros
  if (equal(tok, "__builtin_ctz") || equal(tok, "__builtin_ctzl") ||
      equal(tok, "__builtin_ctzll")) {
    bool is64 = !equal(tok, "__builtin_ctz");
    tok = skip(tok->next, "(");
    Node *arg = assign(&tok, tok);
    if (is64) arg = new_cast(arg, ty_long);
    *rest = skip(tok, ")");
    Node *node = new_unary(ND_BUILTIN_CTZ, arg, tok);
    node->ty = ty_int;
    node->val = is64;
    return node;
  }

  // __builtin_ffs[l|ll](x) — find first set bit (1-indexed, 0 if x==0)
  if (equal(tok, "__builtin_ffs") || equal(tok, "__builtin_ffsl") ||
      equal(tok, "__builtin_ffsll")) {
    bool is64 = !equal(tok, "__builtin_ffs");
    tok = skip(tok->next, "(");
    Node *arg = assign(&tok, tok);
    if (is64) arg = new_cast(arg, ty_long);
    *rest = skip(tok, ")");
    Node *node = new_unary(ND_BUILTIN_FFS, arg, tok);
    node->ty = ty_int;
    node->val = is64;
    return node;
  }

  // __builtin_popcount[l|ll](x) — count set bits
  if (equal(tok, "__builtin_popcount") || equal(tok, "__builtin_popcountl") ||
      equal(tok, "__builtin_popcountll")) {
    tok = skip(tok->next, "(");
    Node *arg = assign(&tok, tok);
    *rest = skip(tok, ")");
    Node *node = new_unary(ND_BUILTIN_POPCOUNT, arg, tok);
    node->ty = ty_int;
    return node;
  }

  // __builtin_parity[l|ll](x) — parity of set bits
  if (equal(tok, "__builtin_parity") || equal(tok, "__builtin_parityl") ||
      equal(tok, "__builtin_parityll")) {
    tok = skip(tok->next, "(");
    Node *arg = assign(&tok, tok);
    *rest = skip(tok, ")");
    Node *node = new_unary(ND_BUILTIN_PARITY, arg, tok);
    node->ty = ty_int;
    return node;
  }

  // __builtin_clrsb[l|ll](x) — count leading redundant sign bits
  if (equal(tok, "__builtin_clrsb") || equal(tok, "__builtin_clrsbl") ||
      equal(tok, "__builtin_clrsbll")) {
    bool is64 = !equal(tok, "__builtin_clrsb");
    tok = skip(tok->next, "(");
    Node *arg = assign(&tok, tok);
    if (is64) arg = new_cast(arg, ty_long);
    *rest = skip(tok, ")");
    Node *node = new_unary(ND_BUILTIN_CLRSB, arg, tok);
    node->ty = ty_int;
    node->val = is64;
    return node;
  }

  // __builtin_bswap16(x) — byte swap 16-bit
  if (equal(tok, "__builtin_bswap16")) {
    tok = skip(tok->next, "(");
    Node *arg = assign(&tok, tok);
    *rest = skip(tok, ")");
    // bswap16: rev16 w0,w0 then extract lower 16 bits
    Node *node = new_unary(ND_BUILTIN_BSWAP32, arg, tok);
    node->ty = ty_ushort;
    node->val = 16; // flag for 16-bit
    return node;
  }

  // __builtin_bswap32(x) — byte swap 32-bit
  if (equal(tok, "__builtin_bswap32")) {
    tok = skip(tok->next, "(");
    Node *arg = assign(&tok, tok);
    *rest = skip(tok, ")");
    Node *node = new_unary(ND_BUILTIN_BSWAP32, arg, tok);
    node->ty = ty_uint;
    return node;
  }

  // __builtin_bswap64(x) — byte swap 64-bit
  if (equal(tok, "__builtin_bswap64")) {
    tok = skip(tok->next, "(");
    Node *arg = assign(&tok, tok);
    *rest = skip(tok, ")");
    Node *node = new_unary(ND_BUILTIN_BSWAP64, arg, tok);
    node->ty = ty_ulong;
    return node;
  }

  // __builtin_classify_type(x) — return integer classification
  if (equal(tok, "__builtin_classify_type")) {
    tok = skip(tok->next, "(");
    Node *arg = assign(&tok, tok);
    add_type(arg);
    *rest = skip(tok, ")");
    // GCC type classification values
    int val = 5; // default: record_type_class
    if (arg->ty) {
      switch (arg->ty->kind) {
      case TY_VOID: val = 0; break;
      case TY_INT: case TY_CHAR: case TY_SHORT: case TY_LONG: case TY_BOOL: case TY_ENUM:
        val = 1; break; // integer_type_class
      case TY_FLOAT: case TY_DOUBLE: case TY_LDOUBLE:
        val = 8; break; // real_type_class
      case TY_PTR: val = 5; break; // pointer_type_class
      case TY_ARRAY: val = 5; break;
      case TY_STRUCT: case TY_UNION: val = 12; break; // record_type_class
      default: val = 5; break;
      }
    }
    return new_num(val, tok);
  }

  // Parenthesized expression
  if (equal(tok, "(") && equal(tok->next, "{")) {
    // GNU statement expression
    Node *node = new_node(ND_STMT_EXPR, tok);
    node->body = compound_stmt(&tok, tok->next)->body;
    *rest = skip(tok, ")");
    return node;
  }

  if (equal(tok, "(")) {
    Node *node = expr(&tok, tok->next);
    *rest = skip(tok, ")");
    return node;
  }

  // _Generic
  if (equal(tok, "_Generic"))
    return generic_selection(rest, tok->next);

  // String literal (with adjacent string concatenation)
  if (tok->kind == TK_STR) {
    // Check if there are adjacent string literals to concatenate
    if (tok->next->kind == TK_STR) {
      // Compute total length
      int total_len = 0;
      for (Token *t = tok; t->kind == TK_STR; t = t->next)
        total_len += t->ty->array_len - 1; // -1 for null terminator
      total_len++; // final null

      char *buf = calloc_checked(1, total_len);
      int offset = 0;
      for (; tok->kind == TK_STR; tok = tok->next) {
        int len = tok->ty->array_len - 1;
        memcpy(buf + offset, tok->str, len);
        offset += len;
      }
      buf[offset] = '\0';

      Type *ty = array_of(ty_char, total_len);
      Obj *var = new_string_literal(buf, ty);
      *rest = tok;
      return new_var_node(var, start);
    }

    Obj *var = new_string_literal(tok->str, tok->ty);
    *rest = tok->next;
    return new_var_node(var, start);
  }

  // Number
  if (tok->kind == TK_NUM) {
    if (is_complex(tok->ty)) {
      // Imaginary literal (e.g., 1.0i): create complex value {0, imag}
      Type *base = tok->ty->base;
      Node *zero = new_node(ND_NUM, tok);
      zero->fval = 0.0;
      zero->val = 0;
      zero->ty = base;
      Node *imag = new_node(ND_NUM, tok);
      imag->fval = tok->fval;
      imag->val = tok->val;
      imag->ty = base;
      *rest = tok->next;
      return new_complex_val(zero, imag, tok->ty, tok);
    }
    Node *node;
    if (is_flonum(tok->ty)) {
      node = new_node(ND_NUM, tok);
      node->fval = tok->fval;
    } else {
      node = new_node(ND_NUM, tok);
      node->val = tok->val;
    }
    node->ty = tok->ty;
    *rest = tok->next;
    return node;
  }

  // __func__ predefined identifier (C99 6.4.2.2)
  if (tok->kind == TK_IDENT &&
      (equal(tok, "__func__") || equal(tok, "__FUNCTION__") || equal(tok, "__PRETTY_FUNCTION__"))) {
    char *name = current_fn ? current_fn->name : "";
    Token *str_tok = tok;
    Obj *var = new_string_literal(strndup_checked(name, strlen(name)),
                                  array_of(ty_char, strlen(name) + 1));
    *rest = tok->next;
    return new_var_node(var, str_tok);
  }

  // Identifier
  if (tok->kind == TK_IDENT) {
    // Look up the variable first
    VarScope *vs = find_var(tok);

    // Function call: ident(args)
    if (equal(tok->next, "(")) {
      // Check if it's a function pointer variable
      if (vs && vs->var && vs->var->ty->kind == TY_PTR &&
          vs->var->ty->base && vs->var->ty->base->kind == TY_FUNC) {
        // Function pointer: parse as variable, then postfix handles the call
        Node *node = new_var_node(vs->var, tok);
        *rest = tok->next;
        // Let the postfix handler deal with the ( call
        return node;
      }
      return funcall(rest, tok, NULL);
    }

    // Variable or enum constant
    if (!vs || (!vs->var && !vs->enum_ty)) {
      error_tok(tok, "undefined variable '%.*s'", tok->len, tok->loc);
    }

    if (vs->var) {
      *rest = tok->next;
      return new_var_node(vs->var, tok);
    }

    // Enum constant
    *rest = tok->next;
    return new_num(vs->enum_val, tok);
  }

  error_tok(tok, "expected an expression");
}

// function call
static Node *funcall(Token **rest, Token *tok, Node *fn) {
  Token *start = tok;
  tok = tok->next->next; // skip name and (

  VarScope *vs = find_var(start);
  if (!vs || !vs->var || vs->var->ty->kind != TY_FUNC) {
    // Implicit function declaration — returns int
    // Create a node anyway
  }

  Type *func_ty = NULL;
  if (vs && vs->var) {
    if (vs->var->ty->kind == TY_FUNC)
      func_ty = vs->var->ty;
    else if (vs->var->ty->kind == TY_PTR && vs->var->ty->base->kind == TY_FUNC)
      func_ty = vs->var->ty->base;
  }

  // Default: implicit function declaration.
  // On ARM64, variadic functions need special calling convention.
  // Detect known variadic functions by name.
  if (!func_ty) {
    char *fn = strndup_checked(start->loc, start->len);
    bool known_variadic = !strcmp(fn, "printf") || !strcmp(fn, "fprintf") ||
                          !strcmp(fn, "sprintf") || !strcmp(fn, "snprintf") ||
                          !strcmp(fn, "scanf") || !strcmp(fn, "sscanf");
    func_ty = func_type(ty_int);
    if (known_variadic) {
      // Create named parameters for the non-variadic portion.
      // printf/scanf: 1 named param (format)
      // fprintf/sprintf/sscanf: 2 named params (dest/stream, format)
      // snprintf: 3 named params (buf, size, format)
      bool two_named = !strcmp(fn, "fprintf") || !strcmp(fn, "sprintf") ||
                       !strcmp(fn, "sscanf");
      bool three_named = !strcmp(fn, "snprintf");
      Type *p1 = copy_type(pointer_to(ty_char));
      p1->next = NULL;
      func_ty->params = p1;
      if (two_named) {
        Type *p2 = copy_type(pointer_to(ty_char));
        p2->next = NULL;
        p1->next = p2;
      } else if (three_named) {
        Type *p2 = copy_type(ty_ulong);
        p2->next = NULL;
        p1->next = p2;
        Type *p3 = copy_type(pointer_to(ty_char));
        p3->next = NULL;
        p2->next = p3;
      }
      func_ty->is_variadic = true;
    } else {
      func_ty->is_variadic = false;
    }
    free(fn);
  }

  Node head = {};
  Node *cur = &head;
  Type *param_ty = func_ty->params;

  while (!equal(tok, ")")) {
    if (cur != &head)
      tok = skip(tok, ",");

    Node *arg = assign(&tok, tok);
    add_type(arg);

    if (param_ty) {
      // Type-check against parameter
      if (param_ty->kind != TY_STRUCT && param_ty->kind != TY_UNION)
        arg = new_cast(arg, param_ty);
      param_ty = param_ty->next;
    } else if (arg->ty->kind == TY_FLOAT) {
      // Default argument promotion: float → double
      arg = new_cast(arg, ty_double);
    }

    cur = cur->next = arg;
  }

  *rest = skip(tok, ")");

  Node *node = new_node(ND_FUNCALL, start);
  node->funcname = strndup_checked(start->loc, start->len);
  node->func_ty = func_ty;
  node->ty = func_ty->return_ty;
  node->args = head.next;

  // Check if calling via variable (function pointer)
  if (vs && vs->var)
    node->var = vs->var;

  return node;
}

//
// Statement parsing
//

static Node *stmt(Token **rest, Token *tok) {
  // Return statement
  if (equal(tok, "return")) {
    Node *node = new_node(ND_RETURN, tok);
    tok = tok->next;

    if (!consume(&tok, tok, ";")) {
      Node *exp = expr(&tok, tok);
      tok = skip(tok, ";");

      // Cast return expression to the function's return type
      if (current_fn && current_fn->ty && current_fn->ty->return_ty) {
        Type *ret_ty = current_fn->ty->return_ty;
        add_type(exp);
        if (ret_ty->kind != TY_VOID)
          exp = new_cast(exp, ret_ty);
      }
      node->lhs = exp;
    }
    *rest = tok;
    return node;
  }

  // If statement
  if (equal(tok, "if")) {
    Node *node = new_node(ND_IF, tok);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    node->then = stmt(&tok, tok);
    if (consume(&tok, tok, "else"))
      node->els = stmt(&tok, tok);
    *rest = tok;
    return node;
  }

  // Switch
  if (equal(tok, "switch")) {
    Node *node = new_node(ND_SWITCH, tok);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");

    Node *sw = current_switch;
    current_switch = node;
    char *brk = brk_label;
    brk_label = node->unique_label = new_unique_name();

    node->then = stmt(rest, tok);

    current_switch = sw;
    brk_label = brk;
    return node;
  }

  // Case
  if (equal(tok, "case")) {
    if (!current_switch)
      error_tok(tok, "stray case");

    Node *node = new_node(ND_CASE, tok);
    int begin = const_expr_val(&tok, tok->next);

    int end;
    if (equal(tok, "...")) {
      // GCC case range extension: case 1 ... 5
      end = const_expr_val(&tok, tok->next);
    } else {
      end = begin;
    }

    tok = skip(tok, ":");
    node->label = new_unique_name();
    node->lhs = stmt(rest, tok);
    node->begin = begin;
    node->end = end;
    node->case_next = current_switch->case_next;
    current_switch->case_next = node;
    return node;
  }

  // Default
  if (equal(tok, "default")) {
    if (!current_switch)
      error_tok(tok, "stray default");
    Node *node = new_node(ND_CASE, tok);
    tok = skip(tok->next, ":");
    node->label = new_unique_name();
    node->lhs = stmt(rest, tok);
    current_switch->default_case = node;
    return node;
  }

  // For loop
  if (equal(tok, "for")) {
    Node *node = new_node(ND_FOR, tok);
    tok = skip(tok->next, "(");

    enter_scope();

    char *brk = brk_label;
    char *cont = cont_label;
    brk_label = node->unique_label = new_unique_name();
    cont_label = new_unique_name();

    if (is_typename(tok)) {
      Type *basety = declspec(&tok, tok, NULL);
      node->init = declaration(&tok, tok, basety, NULL);
    } else {
      node->init = expr_stmt(&tok, tok);
    }

    if (!equal(tok, ";"))
      node->cond = expr(&tok, tok);
    tok = skip(tok, ";");

    if (!equal(tok, ")"))
      node->inc = expr(&tok, tok);
    tok = skip(tok, ")");

    node->then = stmt(rest, tok);

    leave_scope();
    node->cont_label = cont_label;
    brk_label = brk;
    cont_label = cont;
    return node;
  }

  // While loop
  if (equal(tok, "while")) {
    Node *node = new_node(ND_FOR, tok);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");

    char *brk = brk_label;
    char *cont = cont_label;
    brk_label = node->unique_label = new_unique_name();
    cont_label = new_unique_name();

    node->then = stmt(rest, tok);

    node->cont_label = cont_label;
    brk_label = brk;
    cont_label = cont;
    return node;
  }

  // Do-while
  if (equal(tok, "do")) {
    Node *node = new_node(ND_DO, tok);

    char *brk = brk_label;
    char *cont = cont_label;
    brk_label = node->unique_label = new_unique_name();
    cont_label = new_unique_name();

    node->then = stmt(&tok, tok->next);

    node->cont_label = cont_label;
    brk_label = brk;
    cont_label = cont;

    tok = skip(tok, "while");
    tok = skip(tok, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    *rest = skip(tok, ";");
    return node;
  }

  // Goto
  if (equal(tok, "goto")) {
    if (equal(tok->next, "*")) {
      // Computed goto (GCC extension)
      Node *node = new_node(ND_GOTO_EXPR, tok);
      node->lhs = expr(&tok, tok->next->next);
      *rest = skip(tok, ";");
      return node;
    }

    Node *node = new_node(ND_GOTO, tok);
    node->label = strndup_checked(tok->next->loc, tok->next->len);
    node->goto_next = gotos;
    gotos = node;
    *rest = skip(tok->next->next, ";");
    return node;
  }

  // Break
  if (equal(tok, "break")) {
    if (!brk_label)
      error_tok(tok, "stray break");
    Node *node = new_node(ND_GOTO, tok);
    node->unique_label = brk_label;
    *rest = skip(tok->next, ";");
    return node;
  }

  // Continue
  if (equal(tok, "continue")) {
    if (!cont_label)
      error_tok(tok, "stray continue");
    Node *node = new_node(ND_GOTO, tok);
    node->unique_label = cont_label;
    *rest = skip(tok->next, ";");
    return node;
  }

  // ASM
  if (equal(tok, "asm") || equal(tok, "__asm__")) {
    Node *node = new_node(ND_ASM, tok);
    tok = tok->next;
    // Skip volatile qualifier
    if (equal(tok, "volatile") || equal(tok, "__volatile__"))
      tok = tok->next;
    tok = skip(tok, "(");
    if (tok->kind != TK_STR)
      error_tok(tok, "expected string literal");
    node->asm_str = tok->str;
    tok = tok->next;

    // Parse output/input/clobber operands
    // Format: asm(template : outputs : inputs : clobbers)
    // Temporary arrays (max 16 operands each)
    char *out_constraints[16], *in_constraints[16], *clobbers[16];
    Node *out_exprs[16], *in_exprs[16];
    int num_out = 0, num_in = 0, num_clob = 0;
    int section = 0; // 0=outputs, 1=inputs, 2=clobbers

    while (equal(tok, ":")) {
      tok = tok->next;
      if (section < 2) {
        // Parse operand list: "constraint"(expr), ...
        while (!equal(tok, ":") && !equal(tok, ")")) {
          // Optional [name] prefix
          if (equal(tok, "[")) {
            tok = tok->next;
            tok = tok->next; // skip ident
            tok = skip(tok, "]");
          }
          if (tok->kind != TK_STR)
            error_tok(tok, "expected string literal for constraint");
          char *constraint = tok->str;
          tok = tok->next;
          tok = skip(tok, "(");
          Node *operand = expr(&tok, tok);
          add_type(operand);
          tok = skip(tok, ")");

          if (section == 0) {
            if (num_out >= 16)
              error_tok(tok, "too many asm output operands");
            out_constraints[num_out] = constraint;
            out_exprs[num_out] = operand;
            num_out++;
          } else {
            if (num_in >= 16)
              error_tok(tok, "too many asm input operands");
            in_constraints[num_in] = constraint;
            in_exprs[num_in] = operand;
            num_in++;
          }
          if (!consume(&tok, tok, ","))
            break;
        }
      } else {
        // Parse clobber list: "reg", "memory", "cc", ...
        while (!equal(tok, ":") && !equal(tok, ")")) {
          if (tok->kind != TK_STR)
            error_tok(tok, "expected string literal for clobber");
          if (num_clob >= 16)
            error_tok(tok, "too many asm clobbers");
          clobbers[num_clob++] = tok->str;
          tok = tok->next;
          if (!consume(&tok, tok, ","))
            break;
        }
      }
      section++;
    }

    // Copy parsed operands into node
    if (num_out > 0) {
      node->asm_num_outputs = num_out;
      node->asm_output_constraints = calloc_checked(num_out, sizeof(char *));
      node->asm_output_exprs = calloc_checked(num_out, sizeof(Node *));
      for (int i = 0; i < num_out; i++) {
        node->asm_output_constraints[i] = out_constraints[i];
        node->asm_output_exprs[i] = out_exprs[i];
      }
    }
    if (num_in > 0) {
      node->asm_num_inputs = num_in;
      node->asm_input_constraints = calloc_checked(num_in, sizeof(char *));
      node->asm_input_exprs = calloc_checked(num_in, sizeof(Node *));
      for (int i = 0; i < num_in; i++) {
        node->asm_input_constraints[i] = in_constraints[i];
        node->asm_input_exprs[i] = in_exprs[i];
      }
    }
    if (num_clob > 0) {
      node->asm_num_clobbers = num_clob;
      node->asm_clobbers = calloc_checked(num_clob, sizeof(char *));
      for (int i = 0; i < num_clob; i++)
        node->asm_clobbers[i] = clobbers[i];
    }

    *rest = skip(skip(tok, ")"), ";");
    return node;
  }

  // Labeled statement
  if (tok->kind == TK_IDENT && equal(tok->next, ":")) {
    Node *node = new_node(ND_LABEL, tok);
    node->label = strndup_checked(tok->loc, tok->len);
    node->unique_label = new_unique_name();
    node->lhs = stmt(rest, tok->next->next);
    node->goto_next = labels;
    labels = node;
    return node;
  }

  // Compound statement
  if (equal(tok, "{"))
    return compound_stmt(rest, tok);

  return expr_stmt(rest, tok);
}

// compound-stmt = "{" (declaration | stmt)* "}"
static Node *compound_stmt(Token **rest, Token *tok) {
  Node *node = new_node(ND_BLOCK, tok);
  tok = skip(tok, "{");

  Node head = {};
  Node *cur = &head;

  enter_scope();

  while (!equal(tok, "}")) {
    if (is_typename(tok) && !equal(tok->next, ":")) {
      VarAttr attr = {};
      Type *basety = declspec(&tok, tok, &attr);

      if (attr.is_typedef) {
        // typedef
        tok = parse_typedef(tok, basety, &cur);
        continue;
      }

      // Check for function definition (shouldn't appear here but handle gracefully)
      if (is_function_definition(tok, basety)) {
        error_tok(tok, "function definition not allowed here");
      }

      cur = cur->next = declaration(&tok, tok, basety, &attr);
    } else if (equal(tok, "_Static_assert") || equal(tok, "static_assert")) {
      tok = skip(tok->next, "(");
      const_expr_val(&tok, tok);
      tok = skip(tok, ",");
      if (tok->kind == TK_STR)
        tok = tok->next;
      tok = skip(tok, ")");
      tok = skip(tok, ";");
    } else {
      cur = cur->next = stmt(&tok, tok);
    }
  }

  leave_scope();

  node->body = head.next;
  *rest = tok->next;
  return node;
}

static Token *parse_typedef(Token *tok, Type *basety, Node **cur) {
  bool first = true;
  while (!consume(&tok, tok, ";")) {
    if (!first)
      tok = skip(tok, ",");
    first = false;

    Type *ty = declarator(&tok, tok, basety);
    tok = attribute_list(tok, ty, NULL); // handle __attribute__ after typedef
    if (!ty->name)
      error_tok(tok, "typedef name omitted");
    char *name = strndup_checked(ty->name->loc, ty->name->len);
    push_scope(name)->type_def = ty;

    // For VLA typedefs, emit runtime size computation
    if (cur) {
      Node *vla_stmt = compute_vla_size(ty, ty->name);
      if (vla_stmt) {
        *cur = (*cur)->next = vla_stmt;
      }
    }
  }
  return tok;
}

static bool is_function_definition(Token *tok, Type *basety) {
  // Try to parse as declarator and see if followed by {
  if (equal(tok, ";"))
    return false;

  Token *dummy;
  Type *ty = declarator(&dummy, tok, copy_type(basety));
  return ty->kind == TY_FUNC && equal(dummy, "{");
}

// Declaration in local scope
static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr) {
  Node head = {};
  Node *cur = &head;

  // If the basetype (e.g., struct/union) contains VLA members,
  // emit size computation statements now.
  if (type_has_vla(basety) && !basety->vla_size) {
    Node *vla_stmt = compute_vla_size(basety, tok);
    if (vla_stmt)
      cur = cur->next = vla_stmt;
  }

  int i = 0;
  while (!equal(tok, ";")) {
    if (i++ > 0)
      tok = skip(tok, ",");

    Type *ty = declarator(&tok, tok, basety);
    if (ty->kind == TY_VOID)
      error_tok(tok, "variable declared void");
    if (!ty->name)
      error_tok(tok, "variable name omitted");

    // Handle __attribute__ after declarator (e.g., int x __attribute__((aligned(16))) = ...)
    tok = attribute_list(tok, ty, attr);

    if (attr && attr->is_static) {
      // Static local variable → global with mangled name
      Obj *var = new_anon_gvar(ty);
      push_scope(strndup_checked(ty->name->loc, ty->name->len))->var = var;
      if (equal(tok, "="))
        gvar_initializer(&tok, tok->next, var);
      continue;
    }

    if (attr && attr->is_extern) {
      // Extern declaration inside function: reference global, not local
      char *name = strndup_checked(ty->name->loc, ty->name->len);
      Obj *var = new_gvar(name, ty);
      var->is_definition = false;
      continue;
    }

    // Normal local variable
    Obj *var = new_lvar(strndup_checked(ty->name->loc, ty->name->len), ty);

    if (attr && attr->align)
      var->align = attr->align;

    if (equal(tok, "=")) {
      cur = cur->next = lvar_initializer(&tok, tok->next, var);
      continue;
    }

    if (ty->kind == TY_VLA || (ty->vla_size && (ty->kind == TY_STRUCT || ty->kind == TY_UNION))) {
      // VLA or struct/union with VLA members: dynamic stack allocation
      // 1. Compute and store byte size in hidden variable (if not already done)
      if (!ty->vla_size) {
        Node *vla_stmt = compute_vla_size(ty, ty->name);
        if (vla_stmt)
          cur = cur->next = vla_stmt;
      }

      // 2. Create a hidden variable to save sp before allocation.
      //    This allows deallocation when the VLA goes out of scope
      //    or is re-allocated (e.g., via goto loop).
      Obj *saved_sp = new_lvar("", pointer_to(ty_char));

      // 3. Emit VLA stack allocation node
      //    This tells codegen to: restore sp from saved_sp (if non-zero),
      //    save current sp, allocate, and store VLA base pointer.
      Node *vla_node = new_node(ND_VLA_PTR, tok);
      vla_node->var = var;
      // Use lhs to carry the saved_sp variable reference
      Node *sp_var_node = new_node(ND_VAR, tok);
      sp_var_node->var = saved_sp;
      vla_node->lhs = sp_var_node;

      Node *vla_expr = new_node(ND_EXPR_STMT, tok);
      vla_expr->lhs = vla_node;
      cur = cur->next = vla_expr;
      continue;
    }

    if (ty->size < 0)
      error_tok(ty->name, "variable has incomplete type");
  }

  Node *node = new_node(ND_BLOCK, tok);
  node->body = head.next;
  *rest = tok->next;
  return node;
}

// expr-stmt = expr? ";"
static Node *expr_stmt(Token **rest, Token *tok) {
  if (equal(tok, ";")) {
    *rest = tok->next;
    return new_node(ND_BLOCK, tok);
  }

  Node *node = new_node(ND_EXPR_STMT, tok);
  node->lhs = expr(&tok, tok);
  *rest = skip(tok, ";");
  return node;
}

//
// Initializers
//

typedef struct Initializer Initializer;

struct Initializer {
  Initializer *next;
  Type *ty;
  Token *tok;
  bool is_flexible;

  // For aggregate types
  Initializer **children;

  // For leaf initializer
  Node *expr;
};

struct InitDesig {
  InitDesig *next;
  int idx;         // Array index
  Member *member;  // Struct member
  Obj *var;        // Variable being initialized
};

static Initializer *new_initializer(Type *ty, bool is_flexible) {
  Initializer *init = calloc_checked(1, sizeof(Initializer));
  init->ty = ty;

  if (ty->kind == TY_ARRAY) {
    if (is_flexible && ty->size < 0) {
      init->is_flexible = true;
      return init;
    }
    if (is_flexible && ty->array_len == 0) {
      init->is_flexible = true;
      return init;
    }
    init->children = calloc_checked(ty->array_len, sizeof(Initializer *));
    for (int i = 0; i < ty->array_len; i++)
      init->children[i] = new_initializer(ty->base, false);
    return init;
  }

  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    int len = 0;
    for (Member *mem = ty->members; mem; mem = mem->next)
      len++;

    init->children = calloc_checked(len, sizeof(Initializer *));
    int i = 0;
    for (Member *mem = ty->members; mem; mem = mem->next) {
      // Propagate is_flexible to the last member if the struct has a
      // flexible array member, so that the initializer can resize it.
      bool flex = is_flexible && ty->is_flexible && !mem->next;
      init->children[i++] = new_initializer(mem->ty, flex);
    }
    return init;
  }

  return init;
}

static void initializer2(Token **rest, Token *tok, Initializer *init);
static void string_initializer(Token **rest, Token *tok, Initializer *init);
static void array_initializer1(Token **rest, Token *tok, Initializer *init);
static void struct_initializer1(Token **rest, Token *tok, Initializer *init);
static void union_initializer(Token **rest, Token *tok, Initializer *init);
static int count_array_init_elements(Token *tok, Type *ty);

static void initializer2(Token **rest, Token *tok, Initializer *init) {
  // String initializer for char arrays
  if (init->ty->kind == TY_ARRAY && tok->kind == TK_STR &&
      init->ty->base->kind == TY_CHAR) {
    string_initializer(rest, tok, init);
    return;
  }

  if (init->ty->kind == TY_ARRAY) {
    if (equal(tok, "{")) {
      array_initializer1(rest, tok, init);
    } else {
      array_initializer1(rest, tok, init);
    }
    return;
  }

  if (init->ty->kind == TY_STRUCT) {
    if (equal(tok, "{")) {
      struct_initializer1(rest, tok, init);
      return;
    }

    // Struct can be initialized with a single expression
    Node *expr_node = assign(&tok, tok);
    add_type(expr_node);
    if (expr_node->ty->kind == TY_STRUCT) {
      init->expr = expr_node;
      *rest = tok;
      return;
    }

    // Initialize first member — recurse into nested structs/arrays
    // to find the first scalar member
    {
      Initializer *target = init;
      while (target->children && target->children[0] &&
             (target->children[0]->ty->kind == TY_STRUCT ||
              target->children[0]->ty->kind == TY_UNION ||
              target->children[0]->ty->kind == TY_ARRAY)) {
        target = target->children[0];
      }
      if (target->children && target->children[0])
        target->children[0]->expr = expr_node;
    }
    *rest = tok;
    return;
  }

  if (init->ty->kind == TY_UNION) {
    if (!equal(tok, "{") && !equal(tok, ".")) {
      // Union can be initialized with a single expression of the same union type
      Token *save = tok;
      Node *expr_node = assign(&tok, tok);
      add_type(expr_node);
      if (expr_node->ty->kind == TY_UNION) {
        init->expr = expr_node;
        *rest = tok;
        return;
      }
      // Not a union expression; rewind and fall through to member-wise init
      tok = save;
    }
    union_initializer(rest, tok, init);
    return;
  }

  // Scalar
  if (equal(tok, "{")) {
    initializer2(&tok, tok->next, init);
    consume(&tok, tok, ",");
    *rest = skip(tok, "}");
    return;
  }

  init->expr = assign(rest, tok);
}

static void string_initializer(Token **rest, Token *tok, Initializer *init) {
  // Handle adjacent string literal concatenation: "abc" "def" → "abcdef"
  char *str = tok->str;
  int str_len = tok->ty->array_len; // includes null terminator
  Token *start = tok;
  tok = tok->next;

  while (tok->kind == TK_STR) {
    int add_len = tok->ty->array_len - 1; // exclude null
    int new_len = str_len - 1 + add_len + 1;
    char *buf = calloc_checked(1, new_len);
    memcpy(buf, str, str_len - 1);
    memcpy(buf + str_len - 1, tok->str, add_len);
    buf[new_len - 1] = '\0';
    str = buf;
    str_len = new_len;
    tok = tok->next;
  }

  if (init->is_flexible) {
    *init = *new_initializer(array_of(init->ty->base, str_len), false);
  }

  int len = (init->ty->array_len < str_len) ?
            init->ty->array_len : str_len;

  for (int i = 0; i < len; i++) {
    init->children[i]->expr = new_num(str[i], start);
  }
  *rest = tok;
}

static void array_initializer1(Token **rest, Token *tok, Initializer *init) {
  bool has_brace = consume(&tok, tok, "{");

  if (init->is_flexible) {
    int len = count_array_init_elements(tok, init->ty);
    *init = *new_initializer(array_of(init->ty->base, len), false);
  }

  for (int i = 0; i < init->ty->array_len && !is_end(tok); i++) {
    if (i > 0) {
      tok = skip(tok, ",");
      if (is_end(tok)) break; // trailing comma
    }

    // Designated initializer
    if (equal(tok, "[")) {
      i = const_expr_val(&tok, tok->next);
      tok = skip(tok, "]");
      tok = skip(tok, "=");
    }

    if (equal(tok, ".") || equal(tok, "[")) {
      // Nested designator — parse later
    }

    if (i < init->ty->array_len)
      initializer2(&tok, tok, init->children[i]);
    else
      assign(&tok, tok); // skip excess
  }

  if (has_brace) {
    // Skip excess elements and handle trailing comma
    while (!consume(&tok, tok, "}")) {
      if (!consume(&tok, tok, ","))
        tok = tok->next; // skip unknown tokens
      if (equal(tok, "}")) {
        tok = tok->next;
        break;
      }
      if (!equal(tok, "}") && !equal(tok, ","))
        assign(&tok, tok);
    }
  }

  *rest = tok;
}

static bool is_end(Token *tok) {
  return equal(tok, "}") || (tok->kind == TK_EOF);
}

static void struct_initializer1(Token **rest, Token *tok, Initializer *init) {
  bool has_brace = consume(&tok, tok, "{");

  Member *mem = init->ty->members;
  int i = 0;

  while (mem && !is_end(tok)) {
    if (i++ > 0) {
      tok = skip(tok, ",");
      if (is_end(tok)) break; // trailing comma
    }

    // Designated initializer
    if (equal(tok, ".")) {
      tok = tok->next;
      // Find member
      Member *m = init->ty->members;
      for (; m; m = m->next) {
        if (m->name && m->name->len == tok->len &&
            !strncmp(m->name->loc, tok->loc, tok->len)) {
          mem = m;
          break;
        }
      }
      if (!m)
        error_tok(tok, "no such struct member");
      tok = tok->next;
      if (!consume(&tok, tok, "="))
        ; // designator without = is allowed in some modes
    }

    int idx = 0;
    Member *m2 = init->ty->members;
    for (; m2 != mem; m2 = m2->next)
      idx++;

    initializer2(&tok, tok, init->children[idx]);
    mem = mem->next;
  }

  if (has_brace) {
    while (!consume(&tok, tok, "}")) {
      if (!consume(&tok, tok, ",")) tok = tok->next;
      if (equal(tok, "}")) { tok = tok->next; break; }
      if (!equal(tok, "}") && !equal(tok, ","))
        assign(&tok, tok);
    }
  }

  *rest = tok;
}

static void union_initializer(Token **rest, Token *tok, Initializer *init) {
  bool has_brace = consume(&tok, tok, "{");

  // Initialize first member by default
  int idx = 0;

  if (equal(tok, ".")) {
    tok = tok->next;
    Member *m = init->ty->members;
    int j = 0;
    for (; m; m = m->next, j++) {
      if (m->name && m->name->len == tok->len &&
          !strncmp(m->name->loc, tok->loc, tok->len)) {
        idx = j;
        break;
      }
    }
    tok = tok->next;
    consume(&tok, tok, "=");
  }

  initializer2(&tok, tok, init->children[idx]);

  if (has_brace) {
    consume(&tok, tok, ",");
    while (!consume(&tok, tok, "}")) {
      if (!consume(&tok, tok, ",")) tok = tok->next;
      if (equal(tok, "}")) { tok = tok->next; break; }
      if (!equal(tok, "}") && !equal(tok, ","))
        assign(&tok, tok);
    }
  }

  *rest = tok;
}

static int count_array_init_elements(Token *tok, Type *ty) {
  // Note: the outer brace has already been consumed by array_initializer1
  // Don't consume another brace here

  Initializer *dummy = new_initializer(ty->base, false);
  int i;
  for (i = 0; !is_end(tok); i++) {
    if (i > 0) {
      tok = skip(tok, ",");
      if (is_end(tok)) break;
    }
    if (equal(tok, "[")) {
      i = const_expr_val(&tok, tok->next);
      tok = skip(tok, "]");
      tok = skip(tok, "=");
    }
    initializer2(&tok, tok, dummy);
  }
  return i;
}

// Create initialization code from an Initializer tree
static Node *create_lvar_init(Initializer *init, Type *ty, InitDesig *desig, Token *tok) {
  if (ty->kind == TY_ARRAY) {
    Node head = {};
    Node *cur = &head;
    for (int i = 0; i < ty->array_len; i++) {
      InitDesig next = {.next = desig, .idx = i};
      Node *n = create_lvar_init(init->children[i], ty->base, &next, tok);
      if (n) {
        cur->next = n;
        // Advance cur to end of the returned list
        while (cur->next) cur = cur->next;
      }
    }
    return head.next;
  }

  if (ty->kind == TY_STRUCT) {
    // If the entire struct is initialized from a single expression (e.g., s->d),
    // generate a struct assignment instead of per-member init
    if (init->expr) {
      Node *lhs = init_desig_expr(desig, tok);
      return new_binary(ND_ASSIGN, lhs, init->expr, tok);
    }
    Node head = {};
    Node *cur = &head;
    int i = 0;
    for (Member *mem = ty->members; mem; mem = mem->next, i++) {
      InitDesig next = {.next = desig, .member = mem};
      Node *n = create_lvar_init(init->children[i], mem->ty, &next, tok);
      if (n) {
        cur->next = n;
        while (cur->next) cur = cur->next;
      }
    }
    return head.next;
  }

  if (ty->kind == TY_UNION) {
    // Use whichever child has an expr
    int i = 0;
    for (Member *mem = ty->members; mem; mem = mem->next, i++) {
      if (init->children[i]->expr || init->children[i]->children) {
        InitDesig next = {.next = desig, .member = mem};
        return create_lvar_init(init->children[i], mem->ty, &next, tok);
      }
    }
    // Default: init first member
    if (ty->members) {
      InitDesig next = {.next = desig, .member = ty->members};
      return create_lvar_init(init->children[0], ty->members->ty, &next, tok);
    }
    return NULL;
  }

  if (!init->expr)
    return NULL;

  // Build the lvalue designator chain
  Node *lhs = init_desig_expr(desig, tok);
  Node *rhs = init->expr;

  // For complex types, explicitly cast the rhs to match the lhs type
  // (add_type won't auto-cast for complex assignments)
  add_type(lhs);
  add_type(rhs);
  if (is_complex(lhs->ty) && is_complex(rhs->ty) &&
      lhs->ty->base->kind != rhs->ty->base->kind)
    rhs = new_cast(rhs, lhs->ty);
  else if (is_complex(lhs->ty) && !is_complex(rhs->ty))
    rhs = new_cast(rhs, lhs->ty);

  return new_binary(ND_ASSIGN, lhs, rhs, tok);
}

static Node *init_desig_expr(InitDesig *desig, Token *tok) {
  if (desig->var)
    return new_var_node(desig->var, tok);

  if (desig->member) {
    Node *node = new_unary(ND_MEMBER, init_desig_expr(desig->next, tok), tok);
    node->member = desig->member;
    return node;
  }

  // Array element
  Node *node = new_unary(ND_DEREF,
    new_add(init_desig_expr(desig->next, tok),
            new_num(desig->idx, tok), tok), tok);
  return node;
}

static Node *lvar_initializer(Token **rest, Token *tok, Obj *var) {
  Initializer *init = new_initializer(var->ty, true);
  initializer2(rest, tok, init);

  // Handle flexible/incomplete array — update variable type from resolved initializer
  if (var->ty->kind == TY_ARRAY && var->ty->array_len < 0) {
    var->ty = init->ty;
  }

  InitDesig desig = {.var = var};

  // Zero out first, then initialize
  Node *lhs = new_node(ND_MEMZERO, tok);
  lhs->var = var;

  Node *rhs = create_lvar_init(init, var->ty, &desig, tok);

  // Special case: struct/complex assignment from single expression
  if (init->expr) {
    Node *init_val = init->expr;
    add_type(init_val);
    // Cast complex to target type if needed
    if (is_complex(var->ty) && is_complex(init_val->ty) &&
        var->ty->base->kind != init_val->ty->base->kind)
      init_val = new_cast(init_val, var->ty);
    else if (is_complex(var->ty) && !is_complex(init_val->ty))
      init_val = new_cast(init_val, var->ty);
    return new_binary(ND_COMMA, lhs,
                      new_binary(ND_ASSIGN, new_var_node(var, tok), init_val, tok), tok);
  }

  if (!rhs)
    return lhs;

  // Chain: memzero, then ALL assignment nodes via ND_COMMA
  // create_lvar_init returns a linked list via ->next
  // We need to fold them into: COMMA(memzero, COMMA(assign1, COMMA(assign2, ...)))
  Node *node = lhs;
  for (Node *n = rhs; n; ) {
    Node *next = n->next;
    n->next = NULL;
    node = new_binary(ND_COMMA, node, n, tok);
    n = next;
  }
  return node;
}

//
// Global variable initializer
//

static void write_gvar_data(Initializer *init, Type *ty, char *buf, int offset, Relocation **rel_tail);

static void gvar_initializer(Token **rest, Token *tok, Obj *var) {
  Initializer *init = new_initializer(var->ty, true);
  initializer2(rest, tok, init);

  // Handle flexible arrays — update the variable's type from the initializer
  if (var->ty->kind == TY_ARRAY && var->ty->array_len < 0) {
    // The initializer has been resolved with the actual size
    var->ty = init->ty;
  }

  // For structs with flexible array members, compute the total allocation
  // size including the flexible array data.  The struct's ty->size only
  // covers the fixed part; the flexible member's initializer knows the
  // actual element count after initializer2() resolved it.
  int alloc_size = var->ty->size;
  if (var->ty->kind == TY_STRUCT && var->ty->is_flexible) {
    // Find the last member (the flexible array) and its initializer
    int idx = 0;
    Member *last = NULL;
    for (Member *mem = var->ty->members; mem; mem = mem->next) {
      if (!mem->next) last = mem;
      idx++;
    }
    if (last && idx > 0) {
      Initializer *flex_init = init->children[idx - 1];
      if (flex_init->ty && flex_init->ty->kind == TY_ARRAY &&
          flex_init->ty->size > last->ty->size) {
        alloc_size = last->offset + flex_init->ty->size;
      }
    }
  }

  // Allocate data buffer
  Relocation head = {};
  var->rel = &head;

  char *buf = calloc_checked(1, alloc_size);
  Relocation *rel_tail = &head;
  write_gvar_data(init, var->ty, buf, 0, &rel_tail);
  var->init_data = buf;
  var->rel = head.next;

  // Update var's init_data_size so emit_data emits all bytes
  // (including the flexible array data beyond ty->size)
  var->init_data_size = alloc_size;
}

static void write_gvar_data(Initializer *init, Type *ty, char *buf, int offset, Relocation **rel_tail) {
  if (ty->kind == TY_ARRAY) {
    for (int i = 0; i < ty->array_len; i++)
      write_gvar_data(init->children[i], ty->base, buf, offset + ty->base->size * i, rel_tail);
    return;
  }

  if (ty->kind == TY_STRUCT) {
    int i = 0;
    for (Member *mem = ty->members; mem; mem = mem->next, i++) {
      if (mem->is_bitfield) {
        if (!init->children[i]->expr)
          continue;
        Node *expr = init->children[i]->expr;
        add_type(expr);
        int64_t val = eval2(expr, NULL);
        // Mask to bit_width
        uint64_t mask = (mem->bit_width == 64) ? ~(uint64_t)0 : ((uint64_t)1 << mem->bit_width) - 1;
        uint64_t bits = (uint64_t)val & mask;
        // OR into the correct position in the storage unit
        int byte_off = offset + mem->offset;
        int bo = mem->bit_offset;
        // Read current value at byte_off (up to 8 bytes)
        int sz = mem->ty->size;
        uint64_t cur = 0;
        memcpy(&cur, buf + byte_off, sz);
        cur |= (bits << bo);
        memcpy(buf + byte_off, &cur, sz);
      } else {
        // For flexible array members, use the initializer's resolved type
        // which has the actual array length after initialization.
        Type *mem_ty = mem->ty;
        if (!mem->next && ty->is_flexible && init->children[i]->ty &&
            init->children[i]->ty->kind == TY_ARRAY)
          mem_ty = init->children[i]->ty;
        write_gvar_data(init->children[i], mem_ty, buf, offset + mem->offset, rel_tail);
      }
    }
    return;
  }

  if (ty->kind == TY_UNION) {
    int i = 0;
    for (Member *mem = ty->members; mem; mem = mem->next, i++) {
      if (init->children[i]->expr || init->children[i]->children) {
        write_gvar_data(init->children[i], mem->ty, buf, offset, rel_tail);
        return;
      }
    }
    return;
  }

  if (!init->expr)
    return;

  // Evaluate the expression
  Node *expr = init->expr;
  add_type(expr);

  // Complex type: evaluate real and imaginary parts separately
  if (ty->kind == TY_COMPLEX) {
    Type *base = ty->base;
    // Create __real__ and __imag__ nodes to extract parts
    Node *re = new_unary(ND_REAL, expr, expr->tok);
    Node *im = new_unary(ND_IMAG, expr, expr->tok);
    add_type(re);
    add_type(im);
    if (is_flonum(base)) {
      if (base->kind == TY_FLOAT) {
        float fr = eval_double(re);
        float fi = eval_double(im);
        memcpy(buf + offset, &fr, 4);
        memcpy(buf + offset + 4, &fi, 4);
      } else {
        double dr = eval_double(re);
        double di = eval_double(im);
        memcpy(buf + offset, &dr, 8);
        memcpy(buf + offset + 8, &di, 8);
      }
    } else {
      // Integer complex
      int64_t vr = eval(re);
      int64_t vi = eval(im);
      if (base->size <= 4) {
        *(int32_t *)(buf + offset) = vr;
        *(int32_t *)(buf + offset + 4) = vi;
      } else {
        *(int64_t *)(buf + offset) = vr;
        *(int64_t *)(buf + offset + 8) = vi;
      }
    }
    return;
  }

  if (is_flonum(ty)) {
    if (ty->kind == TY_FLOAT) {
      float f = eval_double(expr);
      memcpy(buf + offset, &f, 4);
    } else {
      double d = eval_double(expr);
      memcpy(buf + offset, &d, 8);
    }
    return;
  }

  char **label = NULL;
  int64_t val = eval2(expr, &label);

  if (label) {
    // Relocation needed — add to the relocation list
    Relocation *rel = calloc_checked(1, sizeof(Relocation));
    rel->offset = offset;
    rel->label = label;
    rel->addend = val;
    rel->next = NULL;
    (*rel_tail)->next = rel;
    *rel_tail = rel;
    return;
  }

  // Write integer value
  switch (ty->size) {
  case 1: buf[offset] = val; break;
  case 2: *(uint16_t *)(buf + offset) = val; break;
  case 4: *(uint32_t *)(buf + offset) = val; break;
  case 8: *(uint64_t *)(buf + offset) = val; break;
  }
}

//
// Top-level parsing
//

Obj *parse(Token *tok) {
  globals = NULL;

  enter_scope();

  while (tok->kind != TK_EOF) {
    // Skip standalone __attribute__ at file scope
    if (equal(tok, "__attribute__")) {
      tok = attribute_list(tok, NULL, NULL);
      consume(&tok, tok, ";");
      continue;
    }

    VarAttr attr = {};
    Type *basety = declspec(&tok, tok, &attr);

    // Typedef
    if (attr.is_typedef) {
      tok = parse_typedef(tok, basety, NULL);
      continue;
    }

    // _Static_assert at file scope
    if (equal(tok, ";")) {
      tok = tok->next;
      continue;
    }

    // Function definition or global variable
    Type *ty = declarator(&tok, tok, basety);

    if (ty->kind == TY_FUNC) {
      if (equal(tok, "{")) {
        // Function definition
        tok = function(tok, ty, &attr);
        continue;
      }

      // K&R (old-style) function definition: parameter type declarations
      // appear between the ')' and '{'.  e.g.  int foo(a, b) int a; char *b; { ... }
      // Detect K&R function definition: after the declarator, the next token
      // is a type keyword (not __attribute__, not __asm__, not ;)
      // K&R style: int foo(a, b) int a; char *b; { ... }
      if (is_typename(tok) && !equal(tok, ";") &&
          !equal(tok, "__attribute__") && !equal(tok, "__asm__") &&
          !equal(tok, "__asm") && !equal(tok, "asm")) {
        tok = function(tok, ty, &attr);
        continue;
      }

      // Function declaration
      // Create a global variable for the function
      char *name = strndup_checked(ty->name->loc, ty->name->len);
      Obj *fn = new_gvar(name, ty);
      fn->is_function = true;
      fn->is_definition = false;
      fn->is_static = attr.is_static;

      tok = skip_asm_label(tok);
      tok = attribute_list(tok, ty, &attr);
      tok = skip(tok, ";");
      continue;
    }

    // Bare struct/union definition with no variable name (e.g. "struct foo { ... };")
    if (!ty->name) {
      tok = skip(tok, ";");
      continue;
    }

    // Global variable
    tok = global_variable(tok, ty, basety, &attr);
  }

  leave_scope();
  return globals;
}

static Token *function(Token *tok, Type *fn_ty, VarAttr *attr) {
  char *name = strndup_checked(fn_ty->name->loc, fn_ty->name->len);

  Obj *fn = new_gvar(name, fn_ty);
  fn->is_function = true;
  fn->is_definition = true;
  fn->is_static = attr->is_static;
  fn->is_inline = attr->is_inline;
  fn->is_variadic = fn_ty->is_variadic;

  current_fn = fn;
  locals = NULL;
  gotos = NULL;
  labels = NULL;
  brk_label = NULL;
  cont_label = NULL;
  current_switch = NULL;

  enter_scope();

  // Create parameter variables
  // Note: new_lvar prepends to locals list, so params end up in reverse order.
  // We create them in reverse to get the right order, then set fn->params.
  Type *param_ty = fn_ty->params;
  int nparams = 0;
  for (Type *t = param_ty; t; t = t->next) nparams++;

  // Create an array of param types to process in reverse
  Type **param_arr = calloc_checked(nparams, sizeof(Type *));
  {
    int i = 0;
    for (Type *t = param_ty; t; t = t->next)
      param_arr[i++] = t;
  }

  for (int i = nparams - 1; i >= 0; i--) {
    if (!param_arr[i]->name)
      error_tok(fn_ty->name, "parameter name omitted");
    char *pname = strndup_checked(param_arr[i]->name->loc, param_arr[i]->name->len);
    new_lvar(pname, param_arr[i]);
  }
  free(param_arr);
  fn->params = locals;

  // Handle K&R (old-style) parameter type declarations.
  // If tok does not point at '{', there may be parameter declarations
  // of the form:  int a; char *b;  before the function body.
  if (!equal(tok, "{")) {
    while (!equal(tok, "{")) {
      Type *basety = declspec(&tok, tok, NULL);

      // Parse each declarator in this declaration
      bool first = true;
      while (!consume(&tok, tok, ";")) {
        if (!first)
          tok = skip(tok, ",");
        first = false;

        Type *ty2 = declarator(&tok, tok, basety);
        if (!ty2->name)
          error_tok(tok, "parameter name omitted in K&R declaration");

        char *pname = strndup_checked(ty2->name->loc, ty2->name->len);

        // Find the parameter variable we already created and update its type
        for (Obj *p = fn->params; p; p = p->next) {
          if (!strcmp(p->name, pname)) {
            p->ty = ty2;
            p->align = ty2->align;

            // Also update the function type's param list
            for (Type *pt = fn_ty->params; pt; pt = pt->next) {
              if (pt->name && pt->name->len == ty2->name->len &&
                  !strncmp(pt->name->loc, ty2->name->loc, ty2->name->len)) {
                pt->kind = ty2->kind;
                pt->size = ty2->size;
                pt->align = ty2->align;
                pt->is_unsigned = ty2->is_unsigned;
                pt->base = ty2->base;
                break;
              }
            }
            break;
          }
        }
      }
    }
  }

  // Variadic function: create __va_area__
  if (fn_ty->is_variadic) {
    fn->va_area = new_lvar("__va_area__", pointer_to(ty_char));
    fn->va_area->align = 8;
  }

  // alloca bottom
  fn->alloca_bottom = new_lvar("__alloca_bottom__", pointer_to(ty_char));

  // Parse function body — tok points at "{"
  fn->body = compound_stmt(&tok, tok);

  // Resolve goto labels.
  // First pass: handle ND_LABEL_VAL nodes, which have pre-assigned
  // unique_labels needed for static initializer relocations.
  // The label statement adopts the ND_LABEL_VAL's name.
  for (Node *g = gotos; g; g = g->goto_next) {
    if (g->kind != ND_LABEL_VAL)
      continue;
    for (Node *l = labels; l; l = l->goto_next) {
      if (!strcmp(g->label, l->label)) {
        l->unique_label = g->unique_label;
        break;
      }
    }
  }
  // Second pass: handle ND_GOTO nodes (and ND_LABEL_VAL nodes that
  // didn't find a label yet — though they don't error).
  for (Node *g = gotos; g; g = g->goto_next) {
    if (g->kind == ND_LABEL_VAL)
      continue;
    for (Node *l = labels; l; l = l->goto_next) {
      if (!strcmp(g->label, l->label)) {
        g->unique_label = l->unique_label;
        break;
      }
    }
    if (!g->unique_label && g->kind == ND_GOTO)
      error_tok(g->tok, "use of undeclared label '%s'", g->label);
  }

  fn->locals = locals;

  leave_scope();
  return tok;
}

static Token *global_variable(Token *tok, Type *ty, Type *basety, VarAttr *attr) {
  // ty already has the first declarator parsed (name, type suffix, etc.)
  // basety is the original base type from declspec (before array/pointer suffixes)
  // tok points to what follows the first declarator (=, ;, or ,)

  for (bool first = true; ; first = false) {
    if (!first) {
      // Parse additional declarators after comma, using the original base type
      ty = declarator(&tok, tok, basety);
    }

    if (!ty->name)
      error_tok(tok, "variable name omitted in declaration");
    char *name = strndup_checked(ty->name->loc, ty->name->len);
    Obj *var = new_gvar(name, ty);
    var->is_static = attr->is_static;
    var->is_tls = attr->is_tls;
    if (attr->align)
      var->align = attr->align;
    if (attr->is_extern)
      var->is_definition = false;

    tok = attribute_list(tok, ty, attr);

    if (equal(tok, "=")) {
      var->is_definition = true; // has initializer → is definition even if extern
      gvar_initializer(&tok, tok->next, var);
    } else if (!attr->is_extern && !var->is_tls) {
      var->is_tentative = true;
    }

    if (consume(&tok, tok, ";"))
      break;
    tok = skip(tok, ",");
  }

  return tok;
}
