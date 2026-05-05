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
// declspec (04a_decl.md §B.3, §B.6, §G).
typedef struct {
  bool is_typedef;
  bool is_static;
  bool is_extern;
  bool is_inline;
  bool is_tls;
  int align;
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

//
// Forward declarations for the spine.
//
static Type *declspec(Token **rest, Token *tok, VarAttr *attr);
static Token *parse_typedef(Token *tok, Type *basety);
static Token *function(Token *tok, Type *basety, VarAttr *attr);
static Token *global_variable(Token *tok, Type *basety, VarAttr *attr);
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

#if 0  // unused in spine; revives in tag-zone fills.
static TagScope *find_tag(Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->next)
    for (TagScope *ts = sc->tags; ts; ts = ts->next)
      if (ts->name && tok->len == (int)strlen(ts->name) &&
          !strncmp(tok->loc, ts->name, tok->len))
        return ts;
  return NULL;
}

static VarScope *push_scope(char *name) {
  VarScope *vs = calloc_checked(1, sizeof(VarScope));
  vs->name = name;
  vs->next = scope->vars;
  scope->vars = vs;
  return vs;
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

    // The declarator is parsed inside function() / global_variable()
    // for now (stubs); real impl per 04a §I.2 step 3-6 will route via
    // a dedicated declarator() call here and dispatch on the resulting
    // type kind.
    if (basety->kind == TY_FUNC) {
      tok = function(tok, basety, &attr);
    } else {
      tok = global_variable(tok, basety, &attr);
    }
  }

  // §I.3 — cleanup.
  leave_scope();
  return globals;
}

//
// Stubs.  Each errors out so spine misuse is loud, not silent.
//

static Type *declspec(Token **rest, Token *tok, VarAttr *attr) {
  (void)rest; (void)attr;
  error_tok(tok, "parse_v2: declspec not yet implemented");
}

static Token *parse_typedef(Token *tok, Type *basety) {
  (void)basety;
  error_tok(tok, "parse_v2: parse_typedef not yet implemented");
}

static Token *function(Token *tok, Type *basety, VarAttr *attr) {
  (void)basety; (void)attr;
  error_tok(tok, "parse_v2: function not yet implemented");
}

static Token *global_variable(Token *tok, Type *basety, VarAttr *attr) {
  (void)basety; (void)attr;
  error_tok(tok, "parse_v2: global_variable not yet implemented");
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
