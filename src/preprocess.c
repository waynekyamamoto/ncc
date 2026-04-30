// preprocess.c — C preprocessor
#include "cc.h"
#include <libgen.h>
#include <time.h>
#include <unistd.h>

// Hideset: a set of macro names used to prevent recursive expansion
struct Hideset {
  Hideset *next;
  char *name;
};

// Macro parameter
typedef struct MacroParam MacroParam;
struct MacroParam {
  MacroParam *next;
  char *name;
};

// Macro argument (during expansion)
typedef struct MacroArg MacroArg;
struct MacroArg {
  MacroArg *next;
  char *name;
  bool is_va_args;
  Token *tok;          // original (unexpanded) argument tokens
  Token *expanded;     // pre-expanded argument tokens (for regular substitution)
};

// Macro definition
typedef struct Macro Macro;
typedef Token *MacroHandlerFn(Token *);

struct Macro {
  char *name;
  bool is_objlike;   // true = object-like, false = function-like
  MacroParam *params;
  char *va_args_name;
  Token *body;
  MacroHandlerFn *handler; // for built-in macros
  bool is_locked;    // for preventing recursive expansion
};

// Map of all macros
static HashMap macros;

// #if condition stack
typedef struct CondIncl CondIncl;
struct CondIncl {
  CondIncl *next;
  enum { IN_THEN, IN_ELIF, IN_ELSE } ctx;
  Token *tok;
  bool included;
};
static CondIncl *cond_incl;

// Include paths
StringArray include_paths;

// Pragma handler
static PragmaHandler *pragma_handler;
void set_pragma_handler(PragmaHandler *fn) { pragma_handler = fn; }

// Forward declarations
static Token *preprocess2(Token *tok);
static Macro *find_macro(Token *tok);
static Token *copy_line(Token **rest, Token *tok);
static Token *copy_token_list(Token *tok);
static long const_expr(Token **rest, Token *tok);
static void push_cond_incl(Token *tok, bool included);
static Token *skip_cond_incl(Token *tok);
static long eval_const_expr(Token **rest, Token *tok);

//
// Utility
//

static bool is_hash(Token *tok) {
  return tok->at_bol && equal(tok, "#");
}

static Token *skip_line(Token *tok) {
  if (tok->at_bol)
    return tok;
  warn_tok(tok, "extra token");
  while (!tok->at_bol)
    tok = tok->next;
  return tok;
}

// Copy a token
static Token *copy_token(Token *tok) {
  Token *t = calloc_checked(1, sizeof(Token));
  *t = *tok;
  t->next = NULL;
  return t;
}

// Create a new EOF token
static Token *new_eof(Token *tok) {
  Token *t = copy_token(tok);
  t->kind = TK_EOF;
  t->len = 0;
  return t;
}

//
// Hideset management
//

static Hideset *new_hideset(char *name) {
  Hideset *hs = calloc_checked(1, sizeof(Hideset));
  hs->name = name;
  return hs;
}

static Hideset *hideset_union(Hideset *hs1, Hideset *hs2) {
  Hideset head = {};
  Hideset *cur = &head;

  for (; hs1; hs1 = hs1->next)
    cur = cur->next = new_hideset(hs1->name);
  for (; hs2; hs2 = hs2->next)
    cur = cur->next = new_hideset(hs2->name);
  return head.next;
}

static bool hideset_contains(Hideset *hs, char *s, int len) {
  for (; hs; hs = hs->next)
    if ((int)strlen(hs->name) == len && !strncmp(hs->name, s, len))
      return true;
  return false;
}

static Hideset *hideset_intersection(Hideset *hs1, Hideset *hs2) {
  Hideset head = {};
  Hideset *cur = &head;

  for (; hs1; hs1 = hs1->next)
    if (hideset_contains(hs2, hs1->name, strlen(hs1->name)))
      cur = cur->next = new_hideset(hs1->name);
  return head.next;
}

static Token *add_hideset(Token *tok, Hideset *hs) {
  Token head = {};
  Token *cur = &head;

  for (; tok; tok = tok->next) {
    Token *t = copy_token(tok);
    t->hideset = hideset_union(t->hideset, hs);
    cur = cur->next = t;
  }
  return head.next;
}

// Append tok2 to the end of tok1
static Token *append(Token *tok1, Token *tok2) {
  if (tok1->kind == TK_EOF)
    return tok2;

  Token head = {};
  Token *cur = &head;

  for (; tok1->kind != TK_EOF; tok1 = tok1->next)
    cur = cur->next = copy_token(tok1);
  cur->next = tok2;
  return head.next;
}

//
// Macro operations
//

static Macro *add_macro(char *name, bool is_objlike, Token *body) {
  Macro *m = calloc_checked(1, sizeof(Macro));
  m->name = name;
  m->is_objlike = is_objlike;
  m->body = body;
  hashmap_put(&macros, name, m);
  return m;
}

static Macro *find_macro(Token *tok) {
  if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD)
    return NULL;
  char *name = strndup_checked(tok->loc, tok->len);
  Macro *m = hashmap_get2(&macros, tok->loc, tok->len);
  free(name);
  return m;
}

void define_macro(char *name, char *buf) {
  Token *body = tokenize(new_file("<built-in>", 0, buf));

  // Check if name contains '(' — if so, it's a function-like macro
  char *paren = strchr(name, '(');
  if (paren) {
    // Parse as: name(param1, param2, ...) → body
    char *macro_name = strndup_checked(name, paren - name);

    // Tokenize the parameter list
    char *params_str = strdup(paren); // "(x,y,...)"
    Token *ptok = tokenize(new_file("<built-in>", 0, params_str));
    ptok = skip(ptok, "(");

    MacroParam *param_head = NULL;
    MacroParam **cur = &param_head;
    while (!equal(ptok, ")")) {
      if (param_head) ptok = skip(ptok, ",");
      MacroParam *p = calloc_checked(1, sizeof(MacroParam));
      p->name = strndup_checked(ptok->loc, ptok->len);
      *cur = p;
      cur = &p->next;
      ptok = ptok->next;
    }

    Macro *m = add_macro(macro_name, false, body);
    m->params = param_head;
  } else {
    add_macro(strdup(name), true, body);
  }
}

void undef_macro(char *name) {
  hashmap_delete(&macros, name);
}

//
// Include path handling
//

void add_include_path(char *path) {
  strarray_push(&include_paths, path);
}

static char *search_include_paths(char *filename) {
  if (filename[0] == '/')
    return filename;

  // Search include paths
  for (int i = 0; i < include_paths.len; i++) {
    char *path = format("%s/%s", include_paths.data[i], filename);
    if (fopen(path, "r"))
      return path;
  }
  return NULL;
}

static char *search_include_next(char *filename, int start) {
  for (int i = start; i < include_paths.len; i++) {
    char *path = format("%s/%s", include_paths.data[i], filename);
    if (fopen(path, "r"))
      return path;
  }
  return NULL;
}

// Read an #include filename
static char *read_include_filename(Token **rest, Token *tok, bool *is_dquote) {
  // Pattern 1: #include "foo.h"
  if (tok->kind == TK_STR) {
    *is_dquote = true;
    *rest = skip_line(tok->next);
    return strndup_checked(tok->str, tok->ty->array_len - 1);
  }

  // Pattern 2: #include <foo.h>
  if (equal(tok, "<")) {
    // Read until >
    Token *start = tok;
    for (; !equal(tok, ">"); tok = tok->next)
      if (tok->at_bol || tok->kind == TK_EOF)
        error_tok(start, "expected '>'");

    *is_dquote = false;
    *rest = skip_line(tok->next);

    // Concatenate tokens between < and >
    char *buf;
    size_t buflen;
    FILE *fp = open_memstream(&buf, &buflen);
    for (Token *t = start->next; t != tok;) {
      if (t != start->next && t->has_space)
        fputc(' ', fp);
      fprintf(fp, "%.*s", t->len, t->loc);
      t = t->next;
    }
    fclose(fp);
    return buf;
  }

  // Pattern 3: #include FOO (macro-expanded)
  if (tok->kind == TK_IDENT) {
    Token *tok2 = preprocess2(copy_line(rest, tok));
    return read_include_filename(&tok2, tok2, is_dquote);
  }

  error_tok(tok, "expected a filename");
}

// Copy tokens until end of line
static Token *copy_line(Token **rest, Token *tok) {
  Token head = {};
  Token *cur = &head;

  for (; !tok->at_bol; tok = tok->next)
    cur = cur->next = copy_token(tok);

  cur->next = new_eof(tok);
  *rest = tok;
  return head.next;
}

//
// Constant expression evaluation for #if
//

static Token *read_const_expr(Token **rest, Token *tok) {
  tok = copy_line(rest, tok);

  // Replace __has_attribute(x), __has_builtin(x), __has_feature(x),
  // __has_include(...), __has_include_next(...) with 0 or 1 before
  // the expression is evaluated as a constant.
  for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
    if ((t->kind == TK_IDENT || t->kind == TK_KEYWORD) &&
        ((t->len == 15 && !strncmp(t->loc, "__has_attribute", 15)) ||
         (t->len == 13 && !strncmp(t->loc, "__has_builtin",   13)) ||
         (t->len == 13 && !strncmp(t->loc, "__has_feature",   13)))) {
      // Consume the name and parenthesised argument, yield 0 or 1.
      Token *start = t;
      Token *u = t->next;
      int result = 0;
      if (u && equal(u, "(")) {
        // Collect the single identifier argument.
        Token *arg = u->next;
        int arg_len = 0;
        const char *arg_loc = NULL;
        if (arg && (arg->kind == TK_IDENT || arg->kind == TK_KEYWORD)) {
          arg_loc = arg->loc;
          arg_len = arg->len;
        }
        // Skip to closing paren.
        int level = 1;
        Token *end = u->next;
        while (end && end->kind != TK_EOF && level > 0) {
          if (equal(end, "(")) level++;
          if (equal(end, ")")) level--;
          if (level > 0) end = end->next;
        }

        // __has_attribute: report attributes ncc actually handles.
        if (!strncmp(t->loc, "__has_attribute", 15)) {
          static const char *supported_attrs[] = {
            "packed", "aligned", "section", "unused", "weak", "noinline",
            "noreturn", "always_inline", "cold", "hot", "pure", "const",
            "alias", "used", "warn_unused_result", "noclone", "nothrow",
            "deprecated", "malloc", "flatten", "constructor", "destructor",
            "transparent_union", "returns_nonnull", "may_alias",
            "__packed__", "__aligned__", "__section__", "__unused__",
            "__weak__", "__noinline__", "__noreturn__", "__always_inline__",
            "__cold__", "__pure__", "__const__", "__alias__", "__used__",
            "__deprecated__", "__malloc__",
            NULL
          };
          if (arg_loc && arg_len) {
            for (int i = 0; supported_attrs[i]; i++) {
              if ((int)strlen(supported_attrs[i]) == arg_len &&
                  !strncmp(supported_attrs[i], arg_loc, arg_len)) {
                result = 1;
                break;
              }
            }
          }
        }

        // __has_builtin: report builtins ncc implements.
        if (!strncmp(t->loc, "__has_builtin", 13)) {
          static const char *supported_builtins[] = {
            "__builtin_va_start", "__builtin_va_end", "__builtin_va_arg",
            "__builtin_va_copy", "__builtin_va_list",
            "__builtin_offsetof", "__builtin_types_compatible_p",
            "__builtin_expect",
            "__builtin_unreachable",
            "__builtin_constant_p",
            "__builtin_bswap16", "__builtin_bswap32", "__builtin_bswap64",
            "__builtin_clz", "__builtin_clzl", "__builtin_clzll",
            "__builtin_ctz", "__builtin_ctzl", "__builtin_ctzll",
            "__builtin_popcount", "__builtin_popcountl", "__builtin_popcountll",
            "__builtin_memset", "__builtin_memcpy", "__builtin_memcmp",
            "__builtin_strlen", "__builtin_strcmp",
            "__builtin_frame_address", "__builtin_return_address",
            "__builtin_prefetch",
            "__builtin_alloca",
            NULL
          };
          if (arg_loc && arg_len) {
            for (int i = 0; supported_builtins[i]; i++) {
              if ((int)strlen(supported_builtins[i]) == arg_len &&
                  !strncmp(supported_builtins[i], arg_loc, arg_len)) {
                result = 1;
                break;
              }
            }
          }
        }

        // __has_feature: stub — return 0 for everything (we don't advertise
        // Clang-specific features like address sanitizer, CFI, etc.)
        // result stays 0.

        Token *next = end ? end->next : new_eof(start);
        *start = *new_eof(start);
        start->kind = TK_PP_NUM;
        start->loc = result ? "1" : "0";
        start->len = 1;
        start->next = next;
        t = start;
      }
      continue;
    }

    if ((t->kind == TK_IDENT || t->kind == TK_KEYWORD) &&
        ((t->len == 13 && !strncmp(t->loc, "__has_include", 13)) ||
         (t->len == 18 && !strncmp(t->loc, "__has_include_next", 18)))) {
      Token *start = t;
      Token *u = t->next;
      if (u && equal(u, "(")) {
        // Skip to matching )
        int level = 1;
        Token *end = u->next;
        while (end && level > 0) {
          if (equal(end, "(")) level++;
          if (equal(end, ")")) level--;
          if (level > 0) end = end->next;
        }
        // Replace entire __has_include(...) with 0 (conservative)
        Token *next = end ? end->next : new_eof(start);
        *start = *new_eof(start);
        start->kind = TK_PP_NUM;
        start->loc = "0";
        start->len = 1;
        start->next = next;
        t = start;
      }
    }
  }

  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // "defined(X)" or "defined X"
    if (equal(tok, "defined")) {
      Token *start = tok;
      tok = tok->next;

      bool has_paren = false;
      if (equal(tok, "(")) {
        has_paren = true;
        tok = tok->next;
      }

      if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD)
        error_tok(start, "macro name must be an identifier");

      Macro *m = hashmap_get2(&macros, tok->loc, tok->len);
      tok = tok->next;

      if (has_paren)
        tok = skip(tok, ")");

      Token *t = copy_token(start);
      t->kind = TK_PP_NUM;
      t->loc = m ? "1" : "0";
      t->len = 1;
      cur = cur->next = t;
      continue;
    }

    cur = cur->next = tok;
    tok = tok->next;
  }

  cur->next = tok; // EOF
  return head.next;
}

// Evaluate #if constant expression
static long eval_const_expr(Token **rest, Token *tok) {
  Token *expr = read_const_expr(rest, tok);

  // Expand macros
  expr = preprocess2(expr);

  // Handle 'defined(X)' that appeared after macro expansion, then replace
  // remaining identifiers with 0
  for (Token *t = expr; t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_IDENT && t->len == 7 && !strncmp(t->loc, "defined", 7)) {
      Token *u = t->next;
      bool has_paren = false;
      if (u && equal(u, "(")) { has_paren = true; u = u->next; }
      if (u && u->kind == TK_IDENT) {
        Macro *m = hashmap_get2(&macros, u->loc, u->len);
        Token *end = u->next;
        if (has_paren && end && equal(end, ")")) end = end->next;
        // Replace the entire defined(...) sequence with 0/1
        Token *next = end;
        *t = *new_eof(t);
        t->kind = TK_PP_NUM;
        t->loc = m ? "1" : "0";
        t->len = 1;
        t->next = next;
        continue;
      }
    }
    if (t->kind == TK_IDENT) {
      Token *next = t->next;
      *t = *new_eof(t);
      t->kind = TK_PP_NUM;
      t->loc = "0";
      t->len = 1;
      t->next = next;
    }
  }

  // Convert pp numbers
  convert_pp_tokens(expr);

  Token *rest2;
  long val = const_expr(&rest2, expr);
  if (rest2->kind != TK_EOF)
    error_tok(rest2, "extra token in constant expression");
  return val;
}

// Simple constant expression parser for #if
// This is a minimal implementation.
static long cond(Token **rest, Token *tok);
static long logor(Token **rest, Token *tok);
static long logand(Token **rest, Token *tok);
static long bitor_(Token **rest, Token *tok);
static long bitxor(Token **rest, Token *tok);
static long bitand(Token **rest, Token *tok);
static long equality(Token **rest, Token *tok);
static long relational(Token **rest, Token *tok);
static long shift(Token **rest, Token *tok);
static long add_(Token **rest, Token *tok);
static long mul(Token **rest, Token *tok);
// static long cast(Token **rest, Token *tok);
static long unary(Token **rest, Token *tok);
static long primary(Token **rest, Token *tok);

static long const_expr(Token **rest, Token *tok) {
  return cond(rest, tok);
}

static long cond(Token **rest, Token *tok) {
  long val = logor(&tok, tok);
  if (!equal(tok, "?")) {
    *rest = tok;
    return val;
  }
  tok = tok->next;
  long t = const_expr(&tok, tok);
  tok = skip(tok, ":");
  long f = cond(rest, tok);
  return val ? t : f;
}

static long logor(Token **rest, Token *tok) {
  long val = logand(&tok, tok);
  while (equal(tok, "||")) {
    tok = tok->next;
    long rhs = logand(&tok, tok);
    val = val || rhs;
  }
  *rest = tok;
  return val;
}

static long logand(Token **rest, Token *tok) {
  long val = bitor_(&tok, tok);
  while (equal(tok, "&&")) {
    tok = tok->next;
    long rhs = bitor_(&tok, tok);
    val = val && rhs;
  }
  *rest = tok;
  return val;
}

static long bitor_(Token **rest, Token *tok) {
  long val = bitxor(&tok, tok);
  while (equal(tok, "|")) {
    tok = tok->next;
    val |= bitxor(&tok, tok);
  }
  *rest = tok;
  return val;
}

static long bitxor(Token **rest, Token *tok) {
  long val = bitand(&tok, tok);
  while (equal(tok, "^")) {
    tok = tok->next;
    val ^= bitand(&tok, tok);
  }
  *rest = tok;
  return val;
}

static long bitand(Token **rest, Token *tok) {
  long val = equality(&tok, tok);
  while (equal(tok, "&")) {
    tok = tok->next;
    val &= equality(&tok, tok);
  }
  *rest = tok;
  return val;
}

static long equality(Token **rest, Token *tok) {
  long val = relational(&tok, tok);
  for (;;) {
    if (equal(tok, "==")) {
      tok = tok->next;
      val = val == relational(&tok, tok);
    } else if (equal(tok, "!=")) {
      tok = tok->next;
      val = val != relational(&tok, tok);
    } else {
      *rest = tok;
      return val;
    }
  }
}

static long relational(Token **rest, Token *tok) {
  long val = shift(&tok, tok);
  for (;;) {
    if (equal(tok, "<")) {
      tok = tok->next;
      val = val < shift(&tok, tok);
    } else if (equal(tok, "<=")) {
      tok = tok->next;
      val = val <= shift(&tok, tok);
    } else if (equal(tok, ">")) {
      tok = tok->next;
      val = val > shift(&tok, tok);
    } else if (equal(tok, ">=")) {
      tok = tok->next;
      val = val >= shift(&tok, tok);
    } else {
      *rest = tok;
      return val;
    }
  }
}

static long shift(Token **rest, Token *tok) {
  long val = add_(&tok, tok);
  for (;;) {
    if (equal(tok, "<<")) {
      tok = tok->next;
      val <<= add_(&tok, tok);
    } else if (equal(tok, ">>")) {
      tok = tok->next;
      val >>= add_(&tok, tok);
    } else {
      *rest = tok;
      return val;
    }
  }
}

static long add_(Token **rest, Token *tok) {
  long val = mul(&tok, tok);
  for (;;) {
    if (equal(tok, "+")) {
      tok = tok->next;
      val += mul(&tok, tok);
    } else if (equal(tok, "-")) {
      tok = tok->next;
      val -= mul(&tok, tok);
    } else {
      *rest = tok;
      return val;
    }
  }
}

static long mul(Token **rest, Token *tok) {
  long val = unary(&tok, tok);
  for (;;) {
    if (equal(tok, "*")) {
      tok = tok->next;
      val *= unary(&tok, tok);
    } else if (equal(tok, "/")) {
      tok = tok->next;
      long div = unary(&tok, tok);
      if (div == 0)
        error_tok(tok, "division by zero in preprocessor expression");
      val /= div;
    } else if (equal(tok, "%")) {
      tok = tok->next;
      long div = unary(&tok, tok);
      if (div == 0)
        error_tok(tok, "division by zero in preprocessor expression");
      val %= div;
    } else {
      *rest = tok;
      return val;
    }
  }
}

static long unary(Token **rest, Token *tok) {
  if (equal(tok, "+"))
    return unary(rest, tok->next);
  if (equal(tok, "-"))
    return -unary(rest, tok->next);
  if (equal(tok, "!"))
    return !unary(rest, tok->next);
  if (equal(tok, "~"))
    return ~unary(rest, tok->next);
  return primary(rest, tok);
}

static long primary(Token **rest, Token *tok) {
  if (equal(tok, "(")) {
    long val = const_expr(&tok, tok->next);
    *rest = skip(tok, ")");
    return val;
  }

  if (tok->kind == TK_NUM) {
    // For TK_NUM tokens (from character literals, etc.), use the stored value
    *rest = tok->next;
    return tok->val;
  }

  if (tok->kind == TK_PP_NUM) {
    // Try to parse as integer
    char *end;
    long val = strtol(tok->loc, &end, 0);
    // Skip integer suffix
    while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L')
      end++;
    *rest = tok->next;
    return val;
  }

  error_tok(tok, "expected a number");
}

//
// Macro expansion
//

static Token *stringize(Token *hash, Token *arg) {
  // Create string literal from tokens
  char *buf;
  size_t buflen;
  FILE *fp = open_memstream(&buf, &buflen);

  for (Token *t = arg; t->kind != TK_EOF; t = t->next) {
    if (t != arg && t->has_space)
      fputc(' ', fp);
    fprintf(fp, "%.*s", t->len, t->loc);
  }
  fclose(fp);

  // Escape the string
  char *buf2;
  size_t buflen2;
  FILE *fp2 = open_memstream(&buf2, &buflen2);

  fprintf(fp2, "\"");
  for (int i = 0; buf[i]; i++) {
    if (buf[i] == '\\' || buf[i] == '"')
      fputc('\\', fp2);
    fputc(buf[i], fp2);
  }
  fprintf(fp2, "\"");
  fclose(fp2);
  free(buf);

  Token *tok = tokenize(new_file(hash->file->name, hash->file->file_no, buf2));
  return tok;
}

static Token *paste(Token *lhs, Token *rhs) {
  // Concatenate two tokens
  char *buf = format("%.*s%.*s", lhs->len, lhs->loc, rhs->len, rhs->loc);
  Token *tok = tokenize(new_file(lhs->file->name, lhs->file->file_no, buf));
  if (tok->next->kind != TK_EOF)
    error_tok(lhs, "pasting forms \"%s\", an invalid token", buf);
  return tok;
}

// Find a macro argument by name
static MacroArg *find_arg(MacroArg *args, Token *tok) {
  for (MacroArg *ap = args; ap; ap = ap->next)
    if (tok->len == (int)strlen(ap->name) && !strncmp(tok->loc, ap->name, tok->len))
      return ap;
  return NULL;
}

// Handle a preprocessing directive encountered inside a macro argument list.
// Processes #if/#ifdef/#ifndef/#else/#elif/#endif so that conditional blocks
// within macro arguments are resolved correctly (e.g. the ternary split in
// linux/lib/decompress_inflate.c). Returns tok past the directive.
static Token *handle_pp_directive_in_arg(Token *tok) {
  tok = tok->next; // skip '#'
  if (equal(tok, "ifdef") || equal(tok, "ifndef")) {
    bool is_ifdef = equal(tok, "ifdef");
    bool defined = hashmap_get2(&macros, tok->next->loc, tok->next->len);
    push_cond_incl(tok, is_ifdef ? defined : !defined);
    tok = skip_line(tok->next->next);
    if (is_ifdef ? !defined : defined)
      tok = skip_cond_incl(tok);
    return tok;
  }
  if (equal(tok, "if")) {
    long val = eval_const_expr(&tok, tok->next);
    push_cond_incl(tok, val);
    if (!val)
      tok = skip_cond_incl(tok);
    return tok;
  }
  if (equal(tok, "elif")) {
    if (cond_incl && cond_incl->ctx != IN_ELSE) {
      cond_incl->ctx = IN_ELIF;
      if (!cond_incl->included && eval_const_expr(&tok, tok->next))
        cond_incl->included = true;
      else
        tok = skip_cond_incl(tok);
    }
    return tok;
  }
  if (equal(tok, "else")) {
    if (cond_incl && cond_incl->ctx != IN_ELSE) {
      cond_incl->ctx = IN_ELSE;
      tok = skip_line(tok->next);
      if (cond_incl->included)
        tok = skip_cond_incl(tok);
      else
        cond_incl->included = true;
    }
    return tok;
  }
  if (equal(tok, "endif")) {
    if (cond_incl)
      cond_incl = cond_incl->next;
    tok = skip_line(tok->next);
    return tok;
  }
  // Other directives (#define, #undef, etc.) inside args: skip the line.
  while (!tok->at_bol && tok->kind != TK_EOF)
    tok = tok->next;
  return tok;
}

// Read macro arguments
static MacroArg *read_macro_args(Token **rest, Token *tok, MacroParam *params,
                                  char *va_args_name) {
  Token *start = tok;
  tok = tok->next->next; // skip macro name and '('

  MacroArg head = {};
  MacroArg *cur = &head;

  MacroParam *pp = params;
  for (; pp; pp = pp->next) {
    if (cur != &head)
      tok = skip(tok, ",");

    MacroArg *arg = calloc_checked(1, sizeof(MacroArg));
    arg->name = pp->name;

    // Read tokens until ',' or ')' respecting nesting.
    // Handle preprocessing directives (#if/#ifdef/etc.) in-line so that
    // code like "malloc(cond ? a :\n#ifdef X\n  b);\n#else\n  c);\n#endif"
    // is resolved correctly before the argument is collected.
    Token arg_head = {};
    Token *arg_cur = &arg_head;
    int level = 0;
    while (level > 0 || (!equal(tok, ",") && !equal(tok, ")"))) {
      if (tok->kind == TK_EOF)
        error_tok(start, "unclosed macro argument list");
      if (is_hash(tok)) {
        tok = handle_pp_directive_in_arg(tok);
        continue;
      }
      if (equal(tok, "(")) level++;
      if (equal(tok, ")")) level--;
      arg_cur = arg_cur->next = copy_token(tok);
      tok = tok->next;
    }
    arg_cur->next = new_eof(tok);
    arg->tok = arg_head.next;

    cur = cur->next = arg;
  }

  // Handle variadic arguments
  if (va_args_name) {
    MacroArg *arg = calloc_checked(1, sizeof(MacroArg));
    arg->name = va_args_name;
    arg->is_va_args = true;

    // Skip the separator comma between the last fixed param and the first
    // variadic arg only when fixed params were actually consumed (cur != &head).
    // If cur == &head, the macro has no fixed params, so any leading comma is
    // part of the variadic args (e.g. an empty first variadic argument).
    if (pp == NULL && cur != &head && equal(tok, ","))
      tok = tok->next;

    Token arg_head = {};
    Token *arg_cur = &arg_head;
    int level = 0;
    while (level > 0 || !equal(tok, ")")) {
      if (tok->kind == TK_EOF)
        error_tok(start, "unclosed macro argument list");
      if (is_hash(tok)) {
        tok = handle_pp_directive_in_arg(tok);
        continue;
      }
      if (equal(tok, "(")) level++;
      if (equal(tok, ")")) level--;
      if (level >= 0) {
        arg_cur = arg_cur->next = copy_token(tok);
      }
      tok = tok->next;
    }
    arg_cur->next = new_eof(tok);
    arg->tok = arg_head.next;
    cur = cur->next = arg;
  }

  *rest = skip(tok, ")");
  return head.next;
}

// Substitute macro parameters in the body
static Token *subst(Token *tok, MacroArg *args) {
  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // #arg (stringification)
    if (equal(tok, "#")) {
      MacroArg *arg = find_arg(args, tok->next);
      if (!arg)
        error_tok(tok->next, "'#' is not followed by a macro parameter");
      cur = cur->next = stringize(tok, arg->tok);
      tok = tok->next->next;
      continue;
    }

    // x##y (token pasting), with support for chained A##B##C##D
    if (equal(tok->next, "##")) {
      MacroArg *arg = find_arg(args, tok);
      if (arg) {
        if (arg->tok->kind == TK_EOF) {
          tok = tok->next->next; // skip empty arg and ##
          continue;
        }
        // Copy arg tokens
        for (Token *t = arg->tok; t->kind != TK_EOF; t = t->next)
          cur = cur->next = copy_token(t);
        tok = tok->next->next; // skip arg and ##
      } else {
        cur = cur->next = copy_token(tok);
        tok = tok->next->next; // skip tok and ##
      }

      // Now paste with what follows ##, and keep pasting for chains
      for (;;) {
        MacroArg *rhs = find_arg(args, tok);
        if (rhs) {
          if (rhs->tok->kind == TK_EOF && equal(cur, ",")) {
            // GNU extension: , ## __VA_ARGS__ with empty args → delete comma
            Token *prev = &head;
            while (prev->next && prev->next != cur)
              prev = prev->next;
            prev->next = NULL;
            cur = prev;
          } else if (rhs->tok->kind != TK_EOF && equal(cur, ",")) {
            // , ## __VA_ARGS__ with non-empty args → keep comma, expand args
            for (Token *t = rhs->tok; t->kind != TK_EOF; t = t->next)
              cur = cur->next = copy_token(t);
          } else if (rhs->tok->kind != TK_EOF) {
            *cur = *paste(cur, rhs->tok);
            for (Token *t = rhs->tok->next; t->kind != TK_EOF; t = t->next)
              cur = cur->next = copy_token(t);
          }
          tok = tok->next;
        } else {
          *cur = *paste(cur, tok);
          tok = tok->next;
        }
        // If next is another ##, continue pasting
        if (equal(tok, "##")) {
          tok = tok->next; // skip ##
          continue;
        }
        break;
      }
      continue;
    }

    // Regular macro argument substitution (use pre-expanded arg)
    MacroArg *arg = find_arg(args, tok);
    if (arg) {
      bool first = true;
      for (Token *t = arg->expanded; t->kind != TK_EOF; t = t->next) {
        cur = cur->next = copy_token(t);
        // First token inherits spacing from the parameter token in the body
        if (first) {
          cur->has_space = tok->has_space;
          first = false;
        }
      }
      tok = tok->next;
      continue;
    }

    // __VA_OPT__
    if (equal(tok, "__VA_OPT__") && equal(tok->next, "(")) {
      MacroArg *va = NULL;
      for (MacroArg *ap = args; ap; ap = ap->next)
        if (ap->is_va_args)
          va = ap;

      tok = tok->next->next; // skip __VA_OPT__ and (

      if (va && va->tok->kind != TK_EOF) {
        // VA args are non-empty, include the content
        int level = 1;
        while (level > 0) {
          if (equal(tok, "(")) level++;
          if (equal(tok, ")")) { level--; if (level == 0) break; }
          cur = cur->next = copy_token(tok);
          tok = tok->next;
        }
        tok = tok->next; // skip )
      } else {
        // VA args are empty, skip content
        int level = 1;
        while (level > 0) {
          if (equal(tok, "(")) level++;
          if (equal(tok, ")")) level--;
          tok = tok->next;
        }
      }
      continue;
    }

    // Regular token
    cur = cur->next = copy_token(tok);
    tok = tok->next;
  }

  cur->next = new_eof(tok);
  return head.next;
}

// Deep copy a token list
static Token *copy_token_list(Token *tok) {
  Token head = {};
  Token *cur = &head;
  for (; tok && tok->kind != TK_EOF; tok = tok->next)
    cur = cur->next = copy_token(tok);
  cur->next = new_eof(tok ? tok : cur);
  return head.next;
}

// Expand a single macro invocation
static bool expand_macro(Token **rest, Token *tok) {
  if (hideset_contains(tok->hideset, tok->loc, tok->len))
    return false;

  Macro *m = find_macro(tok);
  if (!m)
    return false;

  // Built-in macro handler
  if (m->handler) {
    *rest = m->handler(tok);
    (*rest)->next = tok->next;
    return true;
  }

  // Object-like macro
  if (m->is_objlike) {
    Hideset *hs = hideset_union(tok->hideset, new_hideset(m->name));
    Token *body = add_hideset(m->body, hs);
    // Set origin only on body tokens (before appending rest)
    for (Token *t = body; t && t->kind != TK_EOF; t = t->next)
      t->origin = tok;
    body = append(body, tok->next);
    *rest = body;
    return true;
  }

  // Function-like macro: must be followed by (
  if (!equal(tok->next, "("))
    return false;

  Token *macro_tok = tok;
  MacroArg *args = read_macro_args(&tok, tok, m->params, m->va_args_name);
  Token *rparen = tok;

  // Pre-expand each argument once (C standard: args expanded before substitution)
  for (MacroArg *ap = args; ap; ap = ap->next)
    ap->expanded = preprocess2(copy_token_list(ap->tok));

  // Substitute parameters
  Hideset *hs = hideset_intersection(macro_tok->hideset, rparen->hideset);
  hs = hideset_union(hs, new_hideset(m->name));
  Token *body = subst(m->body, args);
  body = add_hideset(body, hs);
  // Set origin only on body tokens (before appending rest)
  for (Token *t = body; t && t->kind != TK_EOF; t = t->next)
    t->origin = macro_tok;
  body = append(body, tok);

  *rest = body;
  return true;
}

//
// Built-in macros
//

static Token *file_macro(Token *tmpl) {
  // Return current filename as a string
  while (tmpl->origin)
    tmpl = tmpl->origin;

  char *buf = format("\"%s\"", tmpl->file->display_name);
  return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

static Token *line_macro(Token *tmpl) {
  while (tmpl->origin)
    tmpl = tmpl->origin;

  int line = tmpl->line_no + tmpl->file->line_delta;
  char *buf = format("%d", line);
  return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

static int counter_val;
static Token *counter_macro(Token *tmpl) {
  char *buf = format("%d", counter_val++);
  return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

static Token *timestamp_macro(Token *tmpl) {
  // Return __TIMESTAMP__ as a string
  return tokenize(new_file(tmpl->file->name, tmpl->file->file_no,
                           "\"Unknown\""));
}

static Token *base_file_macro(Token *tmpl) {
  extern char *base_file;
  char *buf = format("\"%s\"", base_file ? base_file : "");
  return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

//
// Init built-in macros
//

void init_macros(void) {
  // Standard predefined macros
  define_macro("__STDC__", "1");
  define_macro("__STDC_VERSION__", "201112L");
  define_macro("__STDC_HOSTED__", "1");
  define_macro("__STDC_NO_ATOMICS__", "1");
  define_macro("__STDC_NO_COMPLEX__", "1");
  define_macro("__STDC_NO_THREADS__", "1");
  define_macro("__STDC_NO_VLA__", "1");
  define_macro("__STDC_UTF_16__", "1");
  define_macro("__STDC_UTF_32__", "1");

  // Platform macros for ARM64 macOS
  define_macro("__LP64__", "1");
  define_macro("__SIZEOF_POINTER__", "8");
  define_macro("__SIZEOF_LONG__", "8");
  define_macro("__SIZEOF_INT__", "4");
  define_macro("__SIZEOF_SHORT__", "2");
  define_macro("__SIZEOF_FLOAT__", "4");
  define_macro("__SIZEOF_DOUBLE__", "8");
  define_macro("__SIZEOF_LONG_DOUBLE__", "8");
  define_macro("__SIZEOF_LONG_LONG__", "8");
  define_macro("__SIZEOF_SIZE_T__", "8");
  define_macro("__SIZEOF_PTRDIFF_T__", "8");
  define_macro("__SIZEOF_WCHAR_T__", "4");

  define_macro("__SIZE_TYPE__", "unsigned long");
  define_macro("__PTRDIFF_TYPE__", "long");
  define_macro("__WCHAR_TYPE__", "int");
  define_macro("__WINT_TYPE__", "int");
  define_macro("__INT8_TYPE__", "signed char");
  define_macro("__INT16_TYPE__", "short");
  define_macro("__INT32_TYPE__", "int");
  define_macro("__INT64_TYPE__", "long");
  define_macro("__UINT8_TYPE__", "unsigned char");
  define_macro("__UINT16_TYPE__", "unsigned short");
  define_macro("__UINT32_TYPE__", "unsigned int");
  define_macro("__UINT64_TYPE__", "unsigned long");
  define_macro("__INTPTR_TYPE__", "long");
  define_macro("__UINTPTR_TYPE__", "unsigned long");
  define_macro("__INTMAX_TYPE__", "long");
  define_macro("__UINTMAX_TYPE__", "unsigned long");

  define_macro("__CHAR_BIT__", "8");
  define_macro("__SCHAR_MAX__", "127");
  define_macro("__SHRT_MAX__", "32767");
  define_macro("__INT_MAX__", "2147483647");
  define_macro("__LONG_MAX__", "9223372036854775807L");
  define_macro("__LONG_LONG_MAX__", "9223372036854775807LL");
  define_macro("__INT8_MAX__", "127");
  define_macro("__INT16_MAX__", "32767");
  define_macro("__INT32_MAX__", "2147483647");
  define_macro("__INT64_MAX__", "9223372036854775807LL");
  define_macro("__UINT8_MAX__", "255");
  define_macro("__UINT16_MAX__", "65535");
  define_macro("__UINT32_MAX__", "4294967295U");
  define_macro("__UINT64_MAX__", "18446744073709551615ULL");
  define_macro("__SIZE_MAX__", "18446744073709551615UL");
  define_macro("__INTMAX_MAX__", "9223372036854775807L");
  define_macro("__UINTMAX_MAX__", "18446744073709551615UL");
  define_macro("__PTRDIFF_MAX__", "9223372036854775807L");
  define_macro("__INTPTR_MAX__", "9223372036854775807L");
  define_macro("__UINTPTR_MAX__", "18446744073709551615UL");

  // Float/double builtin constants
  define_macro("__FLT_MIN__", "1.17549435e-38F");
  define_macro("__FLT_MAX__", "3.40282347e+38F");
  define_macro("__FLT_EPSILON__", "1.19209290e-07F");
  define_macro("__FLT_DENORM_MIN__", "1.40129846e-45F");
  define_macro("__FLT_HAS_INFINITY__", "1");
  define_macro("__FLT_HAS_QUIET_NAN__", "1");
  define_macro("__DBL_MIN__", "2.2250738585072014e-308");
  define_macro("__DBL_MAX__", "1.7976931348623157e+308");
  define_macro("__DBL_EPSILON__", "2.2204460492503131e-16");
  define_macro("__DBL_DENORM_MIN__", "4.9406564584124654e-324");
  define_macro("__LDBL_MIN__", "2.2250738585072014e-308L");
  define_macro("__LDBL_MAX__", "1.7976931348623157e+308L");
  define_macro("__LDBL_EPSILON__", "2.2204460492503131e-16L");
  define_macro("__FLT_MANT_DIG__", "24");
  define_macro("__DBL_MANT_DIG__", "53");
  define_macro("__LDBL_MANT_DIG__", "53");
  define_macro("__FLT_DIG__", "6");
  define_macro("__DBL_DIG__", "15");
  define_macro("__LDBL_DIG__", "15");
  define_macro("__FLT_MIN_EXP__", "(-125)");
  define_macro("__FLT_MAX_EXP__", "128");
  define_macro("__DBL_MIN_EXP__", "(-1021)");
  define_macro("__DBL_MAX_EXP__", "1024");
  define_macro("__LDBL_MIN_EXP__", "(-1021)");
  define_macro("__LDBL_MAX_EXP__", "1024");
  define_macro("__FLT_MIN_10_EXP__", "(-37)");
  define_macro("__FLT_MAX_10_EXP__", "38");
  define_macro("__DBL_MIN_10_EXP__", "(-307)");
  define_macro("__DBL_MAX_10_EXP__", "308");
  define_macro("__FINITE_MATH_ONLY__", "0");
  define_macro("__SCHAR_MAX__", "127");

  define_macro("__ORDER_LITTLE_ENDIAN__", "1234");
  define_macro("__ORDER_BIG_ENDIAN__", "4321");
  define_macro("__BYTE_ORDER__", "1234");
  define_macro("__LITTLE_ENDIAN__", "1");

  // ARM64 / Apple platform
  define_macro("__aarch64__", "1");
  define_macro("__arm64__", "1");
  define_macro("__arm64", "1");
  define_macro("__AARCH64EL__", "1");
  // gcc-equivalent ARM architecture predefines.
  // NetBSD's <arm/cdefs.h> uses these to set _ARM_ARCH_8/_ARM_ARCH_7,
  // which in turn select AArch64 dsb/dmb/isb instead of AArch32 mcr p15.
  define_macro("__ARM_ARCH", "8");
  define_macro("__ARM_ARCH_8A__", "1");
  define_macro("__ARM_PCS_AAPCS64", "1");
  define_macro("__APPLE__", "1");
  define_macro("__MACH__", "1");
  define_macro("__DARWIN_C_LEVEL", "900000L");

  // Apple target conditionals (needed for TargetConditionals.h)
  define_macro("TARGET_OS_MAC", "1");
  define_macro("TARGET_OS_OSX", "1");
  define_macro("TARGET_OS_IPHONE", "0");
  define_macro("TARGET_OS_IOS", "0");
  define_macro("TARGET_OS_WATCH", "0");
  define_macro("TARGET_OS_TV", "0");
  define_macro("TARGET_OS_SIMULATOR", "0");
  define_macro("TARGET_OS_EMBEDDED", "0");
  define_macro("TARGET_CPU_ARM64", "1");
  define_macro("TARGET_CPU_ARM", "0");
  define_macro("TARGET_CPU_X86", "0");
  define_macro("TARGET_CPU_X86_64", "0");
  define_macro("TARGET_RT_LITTLE_ENDIAN", "1");
  define_macro("TARGET_RT_BIG_ENDIAN", "0");
  define_macro("TARGET_RT_64_BIT", "1");
  define_macro("TARGET_RT_MAC_MACHO", "1");
  define_macro("TARGET_OS_MACCATALYST", "0");
  define_macro("TARGET_OS_DRIVERKIT", "0");

  // GCC compatibility — advertise GCC 12 for modern kernel feature paths
  define_macro("__GNUC__", "12");
  define_macro("__GNUC_MINOR__", "1");
  define_macro("__GNUC_PATCHLEVEL__", "0");
  define_macro("__GNUC_STDC_INLINE__", "1");
  define_macro("__VERSION__", "\"ncc 1.0 compatible\"");

  // GCC atomic memory order constants
  define_macro("__ATOMIC_RELAXED", "0");
  define_macro("__ATOMIC_CONSUME", "1");
  define_macro("__ATOMIC_ACQUIRE", "2");
  define_macro("__ATOMIC_RELEASE", "3");
  define_macro("__ATOMIC_ACQ_REL", "4");
  define_macro("__ATOMIC_SEQ_CST", "5");

  // GCC atomic builtins (single-threaded stubs)
  define_macro("__atomic_load_n(p,o)", "(*(p))");
  define_macro("__atomic_store_n(p,v,o)", "(*(p)=(v))");
  define_macro("__atomic_exchange_n(p,v,o)", "__sync_lock_test_and_set(p,v)");
  define_macro("__atomic_compare_exchange_n(p,e,d,w,s,f)", "__sync_bool_compare_and_swap(p,*(e),d)");
  define_macro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1", "1");
  define_macro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_2", "1");
  define_macro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4", "1");
  define_macro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8", "1");

  define_macro("__alignof__", "_Alignof");
  define_macro("__const__", "const");
  define_macro("__const", "const");
  define_macro("__inline__", "inline");
  define_macro("__inline", "inline");
  define_macro("__uint128_t", "unsigned long");  // simplified: treat as 64-bit
  define_macro("__int128_t", "long");            // simplified
  define_macro("__int128", "long");              // simplified
  define_macro("_Float16", "float");             // simplified: treat as float
  define_macro("__volatile__", "volatile");
  define_macro("__volatile", "volatile");
  define_macro("__attribute", "__attribute__");
  define_macro("__signed__", "signed");
  define_macro("__signed", "signed");
  define_macro("__restrict__", "restrict");
  define_macro("__restrict", "restrict");
  define_macro("__extension__", "");
  define_macro("__complex", "_Complex");
  define_macro("__real", "__real__");
  define_macro("__imag", "__imag__");
  define_macro("__asm", "asm");
  define_macro("__builtin_va_list", "void *");

  // Builtin function-like macros
  define_macro("__builtin_expect(x,y)", "(x)");
  define_macro("__builtin_fabsf(x)", "fabsf(x)");
  define_macro("__builtin_fabs(x)", "fabs(x)");
  define_macro("__builtin_fabsl(x)", "fabsl(x)");
  define_macro("__builtin_inff()", "__FLT_MAX__");
  define_macro("__builtin_inf()", "__DBL_MAX__");
  define_macro("__builtin_infl()", "__LDBL_MAX__");
  define_macro("__builtin_nanf(x)", "(0.0f/0.0f)");
  define_macro("__builtin_nan(x)", "(0.0/0.0)");
  define_macro("__builtin_huge_valf()", "__FLT_MAX__");
  define_macro("__builtin_huge_val()", "__DBL_MAX__");
  define_macro("__builtin_offsetof(type,member)", "((unsigned long)&((type*)0)->member)");
  // __builtin_types_compatible_p is handled in parse.c primary()
  // __builtin_constant_p is handled in parse.c primary()
  define_macro("__builtin_unreachable()", "((void)0)");
  define_macro("__builtin_assume(x)", "((void)0)");
  define_macro("__builtin_trap()", "abort()");
  // Bit manipulation builtins - handled as special cases in parse.c primary()
  // We do NOT define these as macros; they are parsed directly.
  // Define builtin string/memory functions as object-like macros.
  // Object-like macros work better than function-like when the builtins
  // are expanded through macro chains (e.g., #define strcmp __builtin_strcmp).
  define_macro("__builtin_memset", "memset");
  define_macro("__builtin_memcpy", "memcpy");
  define_macro("__builtin_memmove", "memmove");
  define_macro("__builtin_memcmp", "memcmp");
  define_macro("__builtin_strcmp", "strcmp");
  define_macro("__builtin_strncmp", "strncmp");
  define_macro("__builtin_strcpy", "strcpy");
  define_macro("__builtin_strncpy", "strncpy");
  define_macro("__builtin_strlen", "strlen");
  define_macro("__builtin_abort()", "abort()");
  define_macro("__builtin_exit(n)", "exit(n)");
  define_macro("__builtin_malloc", "malloc");
  define_macro("__builtin_calloc", "calloc");
  define_macro("__builtin_free", "free");
  // __builtin_printf/abort: redefine as the libc names.
  // Tests that use these usually also have "void abort(void);" declared,
  // but printf is often undeclared. For undeclared functions, our compiler
  // creates variadic implicit decls — which puts all args on stack.
  // printf needs its first arg (format) in x0 register.
  // Workaround: define these to themselves so they link to libc directly.
  // These map to libc names. The tests usually have "void abort(void);"
  // declared already. printf is tricky on ARM64 because it's variadic.
  define_macro("__builtin_printf", "printf");
  define_macro("__builtin_sprintf", "sprintf");
  define_macro("__builtin_putchar(c)", "putchar(c)");
  define_macro("__builtin_puts(s)", "puts(s)");
  define_macro("__builtin_signbit(x)", "((x) < 0)");
  define_macro("__builtin_signbitf(x)", "((x) < 0)");
  define_macro("__builtin_signbitl(x)", "((x) < 0)");

  // Date/time macros
  {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    static const char *mon[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    define_macro("__DATE__", format("\"%s %2d %d\"",
                 mon[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900));
    define_macro("__TIME__", format("\"%02d:%02d:%02d\"",
                 tm->tm_hour, tm->tm_min, tm->tm_sec));
  }

  // _Pragma(x) — C99 operator: equivalent to #pragma x.
  // ncc ignores all pragmas so this is a no-op.
  define_macro("_Pragma(x)", "");

  // Built-in handler macros
  {
    Macro *m;
    m = add_macro("__FILE__", true, NULL);
    m->handler = file_macro;
    m = add_macro("__LINE__", true, NULL);
    m->handler = line_macro;
    m = add_macro("__COUNTER__", true, NULL);
    m->handler = counter_macro;
    m = add_macro("__TIMESTAMP__", true, NULL);
    m->handler = timestamp_macro;
    m = add_macro("__BASE_FILE__", true, NULL);
    m->handler = base_file_macro;
  }
}

//
// Preprocessor directive handling
//

static Token *include_file(Token *tok, char *path, Token *filename_tok) {
  Token *tok2 = tokenize_file(path);
  if (!tok2)
    error_tok(filename_tok, "%s: cannot open file: %s", path, strerror(errno));
  // Instead of copying all tokens, find the EOF token of the included file
  // and link it directly to the remaining token stream
  Token *t = tok2;
  while (t->kind != TK_EOF)
    t = t->next;
  // Replace the EOF with a link to the rest
  *t = *tok;
  // But we need to preserve at_bol for the first token after include
  return tok2;
}

// Skip until matching #endif for excluded conditional blocks
static Token *skip_cond_incl2(Token *tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) &&
        (equal(tok->next, "if") || equal(tok->next, "ifdef") ||
         equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(tok->next->next);
      continue;
    }
    if (is_hash(tok) && equal(tok->next, "endif"))
      return tok->next->next;
    tok = tok->next;
  }
  return tok;
}

static Token *skip_cond_incl(Token *tok) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) &&
        (equal(tok->next, "if") || equal(tok->next, "ifdef") ||
         equal(tok->next, "ifndef"))) {
      tok = skip_cond_incl2(tok->next->next);
      continue;
    }
    if (is_hash(tok) &&
        (equal(tok->next, "elif") || equal(tok->next, "else") ||
         equal(tok->next, "endif")))
      break;
    tok = tok->next;
  }
  return tok;
}

static void push_cond_incl(Token *tok, bool included) {
  CondIncl *ci = calloc_checked(1, sizeof(CondIncl));
  ci->next = cond_incl;
  ci->ctx = IN_THEN;
  ci->tok = tok;
  ci->included = included;
  cond_incl = ci;
}

// Read a #define directive
static Token *read_macro_definition(Token **rest, Token *tok) {
  if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD)
    error_tok(tok, "macro name must be an identifier");
  char *name = strndup_checked(tok->loc, tok->len);
  tok = tok->next;

  if (!tok->has_space && equal(tok, "(")) {
    // Function-like macro
    tok = tok->next;

    MacroParam head = {};
    MacroParam *cur = &head;
    char *va_args_name = NULL;

    while (!equal(tok, ")")) {
      if (cur != &head)
        tok = skip(tok, ",");

      if (equal(tok, "...")) {
        va_args_name = "__VA_ARGS__";
        tok = tok->next;
        break;
      }

      if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD)
        error_tok(tok, "expected parameter name");

      // Check for named variadic parameter: name...
      if (equal(tok->next, "...")) {
        va_args_name = strndup_checked(tok->loc, tok->len);
        tok = tok->next->next;
        break;
      }

      MacroParam *param = calloc_checked(1, sizeof(MacroParam));
      param->name = strndup_checked(tok->loc, tok->len);
      cur = cur->next = param;
      tok = tok->next;
    }

    tok = skip(tok, ")");

    Macro *m = add_macro(name, false, copy_line(rest, tok));
    m->params = head.next;
    m->va_args_name = va_args_name;
    return *rest;
  }

  // Object-like macro
  add_macro(name, true, copy_line(rest, tok));
  return *rest;
}

//
// Main preprocessor loop
//

long pp_token_count;
static Token *preprocess2(Token *tok) {
  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    pp_token_count++;
    // Expand macros
    if (expand_macro(&tok, tok))
      continue;

    // Non-directive lines
    if (!is_hash(tok)) {
      tok->line_delta = tok->file->line_delta;
      cur = cur->next = tok;
      tok = tok->next;
      continue;
    }

    Token *start = tok;
    tok = tok->next;

    if (equal(tok, "include")) {
      bool is_dquote;
      char *filename = read_include_filename(&tok, tok->next, &is_dquote);

      // Search for the file
      char *path = NULL;
      if (is_dquote) {
        // Try relative to current file first
        char *dir = strdup(start->file->name);
        dir = dirname(dir);
        char *try_path = format("%s/%s", dir, filename);
        FILE *fp = fopen(try_path, "r");
        if (fp) {
          fclose(fp);
          path = try_path;
        }
      }
      if (!path)
        path = search_include_paths(filename);
      if (!path)
        error_tok(start, "'%s': file not found", filename);

      tok = include_file(tok, path, start->next);
      continue;
    }

    if (equal(tok, "include_next")) {
      bool is_dquote;
      char *filename = read_include_filename(&tok, tok->next, &is_dquote);

      // Find which include path the current file was found in,
      // then start searching from the next index onwards.
      char *cur_file = start->file->name;
      char *cur_dir = dirname(strdup(cur_file));
      int start_idx = 0;
      for (int i = 0; i < include_paths.len; i++) {
        if (!strcmp(include_paths.data[i], cur_dir)) {
          start_idx = i + 1;
          break;
        }
      }

      char *path = search_include_next(filename, start_idx);
      if (!path)
        error_tok(start, "'%s': file not found", filename);

      tok = include_file(tok, path, start->next);
      continue;
    }

    if (equal(tok, "define")) {
      read_macro_definition(&tok, tok->next);
      continue;
    }

    if (equal(tok, "undef")) {
      tok = tok->next;
      if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD)
        error_tok(tok, "macro name must be an identifier");
      undef_macro(strndup_checked(tok->loc, tok->len));
      tok = skip_line(tok->next);
      continue;
    }

    if (equal(tok, "if")) {
      long val = eval_const_expr(&tok, tok->next);
      push_cond_incl(start, val);
      if (!val)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "ifdef")) {
      bool defined = hashmap_get2(&macros, tok->next->loc, tok->next->len);
      push_cond_incl(tok, defined);
      tok = skip_line(tok->next->next);
      if (!defined)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "ifndef")) {
      bool defined = hashmap_get2(&macros, tok->next->loc, tok->next->len);
      push_cond_incl(tok, !defined);
      tok = skip_line(tok->next->next);
      if (defined)
        tok = skip_cond_incl(tok);
      continue;
    }

    if (equal(tok, "elif")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        error_tok(start, "stray #elif");
      cond_incl->ctx = IN_ELIF;

      if (!cond_incl->included && eval_const_expr(&tok, tok->next)) {
        cond_incl->included = true;
      } else {
        tok = skip_cond_incl(tok);
      }
      continue;
    }

    if (equal(tok, "else")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        error_tok(start, "stray #else");
      cond_incl->ctx = IN_ELSE;
      tok = skip_line(tok->next);

      if (cond_incl->included) {
        tok = skip_cond_incl(tok);
      } else {
        cond_incl->included = true;
      }
      continue;
    }

    if (equal(tok, "endif")) {
      if (!cond_incl)
        error_tok(start, "stray #endif");
      cond_incl = cond_incl->next;
      tok = skip_line(tok->next);
      continue;
    }

    if (equal(tok, "line")) {
      Token *t = preprocess2(copy_line(&tok, tok->next));
      convert_pp_tokens(t);
      if (t->kind != TK_NUM || t->ty->kind != TY_INT)
        error_tok(t, "invalid line marker");
      start->file->line_delta = t->val - start->line_no;
      if (t->next->kind == TK_STR)
        start->file->display_name = t->next->str;
      continue;
    }

    if (equal(tok, "pragma")) {
      tok = tok->next;
      // Handle #pragma once
      if (equal(tok, "once")) {
        // Simple: just mark and skip
        tok = skip_line(tok->next);
        continue;
      }
      // Handle #pragma pack
      // For now, skip unknown pragmas
      while (!tok->at_bol && tok->kind != TK_EOF)
        tok = tok->next;
      continue;
    }

    if (equal(tok, "error")) {
      error_tok(tok, "");
    }

    if (equal(tok, "warning")) {
      warn_tok(tok->next, "");
      tok = skip_line(tok->next);
      continue;
    }

    // Null directive
    if (tok->at_bol)
      continue;

    error_tok(tok, "invalid preprocessor directive");
  }

  cur->next = tok;
  return head.next;
}

Token *preprocess(Token *tok) {
  pp_token_count = 0;
  tok = preprocess2(tok);
  if (cond_incl)
    error_tok(cond_incl->tok, "unterminated conditional directive");
  convert_pp_tokens(tok);
  return tok;
}
