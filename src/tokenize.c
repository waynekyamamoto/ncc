// tokenize.c — C lexer / tokenizer
#include "cc.h"

// Input files
static File **input_files;
static int num_input_files;

File **get_input_files(void) {
  return input_files;
}

File *get_file(Token *tok) {
  return tok->file;
}

// Error reporting

// Current token being processed (for error messages)
static File *current_file;

static void verror_at(int line_no, char *loc, const char *fmt, va_list ap) {
  // Find the line containing `loc`
  char *input = current_file->contents;
  char *line = loc;
  while (input < line && line[-1] != '\n')
    line--;

  char *end = loc;
  while (*end && *end != '\n')
    end++;

  // Print the source line
  int indent = fprintf(stderr, "%s:%d: ", current_file->display_name, line_no);
  fprintf(stderr, "%.*s\n", (int)(end - line), line);

  // Print the error indicator
  int pos = loc - line + indent;
  fprintf(stderr, "%*s", pos, "");
  fprintf(stderr, "^ ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

_Noreturn void error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

_Noreturn void error_at(const char *loc, const char *fmt, ...) {
  int line_no = 1;
  for (char *p = current_file->contents; p < loc; p++)
    if (*p == '\n')
      line_no++;

  va_list ap;
  va_start(ap, fmt);
  verror_at(line_no, (char *)loc, fmt, ap);
  va_end(ap);
  exit(1);
}

_Noreturn void error_tok(Token *tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  if (tok && tok->file) {
    File *saved = current_file;
    current_file = tok->file;
    verror_at(tok->line_no, tok->loc, fmt, ap);
    current_file = saved;
  } else {
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
  }
  va_end(ap);
  exit(1);
}

void warn_tok(Token *tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  if (tok && tok->file) {
    File *saved = current_file;
    current_file = tok->file;
    verror_at(tok->line_no, tok->loc, fmt, ap);
    current_file = saved;
  } else {
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
  }
  va_end(ap);
}

// Token operations

bool equal(Token *tok, const char *op) {
  return (int)strlen(op) == tok->len && !strncmp(tok->loc, op, tok->len);
}

Token *skip(Token *tok, const char *op) {
  if (!equal(tok, op))
    error_tok(tok, "expected '%s'", op);
  return tok->next;
}

bool consume(Token **rest, Token *tok, const char *str) {
  if (equal(tok, str)) {
    *rest = tok->next;
    return true;
  }
  *rest = tok;
  return false;
}

// Create a new token
static Token *new_token(TokenKind kind, char *start, char *end) {
  Token *tok = calloc_checked(1, sizeof(Token));
  tok->kind = kind;
  tok->loc = start;
  tok->len = end - start;
  tok->file = current_file;
  tok->at_bol = false;
  tok->has_space = false;
  return tok;
}

// Check if p starts with the given string
static bool startswith(const char *p, const char *q) {
  return strncmp(p, q, strlen(q)) == 0;
}

// Read an identifier and return its length
static int read_ident(char *start) {
  char *p = start;
  uint32_t c = decode_utf8(&p, start);
  if (!is_ident1(c))
    return 0;

  while (*p) {
    char *q;
    c = decode_utf8(&q, p);
    if (!is_ident2(c))
      break;
    p = q;
  }
  return p - start;
}

// Read a punctuator and return its length
static int read_punct(char *p) {
  // 3-character punctuators
  static const char *kw3[] = {
    "<<=", ">>=", "...", NULL
  };
  for (int i = 0; kw3[i]; i++)
    if (startswith(p, kw3[i]))
      return 3;

  // 2-character punctuators
  static const char *kw2[] = {
    "==", "!=", "<=", ">=", "->", "+=", "-=", "*=", "/=",
    "%=", "&=", "|=", "^=", "&&", "||", "<<", ">>", "++",
    "--", "##", NULL
  };
  for (int i = 0; kw2[i]; i++)
    if (startswith(p, kw2[i]))
      return 2;

  return ispunct(*p) ? 1 : 0;
}

// Is this a keyword?
static bool is_keyword(Token *tok) {
  static const char *kw[] = {
    "return", "if", "else", "for", "while", "do", "sizeof",
    "struct", "union", "enum", "typedef", "static", "extern",
    "inline", "_Noreturn", "void", "char", "short", "int",
    "long", "float", "double", "signed", "unsigned", "_Bool",
    "const", "volatile", "restrict", "_Atomic", "_Alignof",
    "_Alignas", "auto", "register", "switch", "case", "default",
    "break", "continue", "goto", "__attribute__", "_Thread_local",
    "__thread", "typeof", "__typeof", "__typeof__", "asm", "__asm__",
    "_Static_assert", "static_assert",
    "_Generic",
    "_Complex", "__complex__", "__real__", "__imag__",
    NULL
  };

  for (int i = 0; kw[i]; i++)
    if (equal(tok, kw[i]))
      return true;
  return false;
}

// Read an escaped character in a string/char literal
static int read_escaped_char(char **new_pos, char *p) {
  // Octal
  if ('0' <= *p && *p <= '7') {
    int c = *p++ - '0';
    if ('0' <= *p && *p <= '7')
      c = (c << 3) + (*p++ - '0');
    if ('0' <= *p && *p <= '7')
      c = (c << 3) + (*p++ - '0');
    *new_pos = p;
    return c;
  }

  // Hex
  if (*p == 'x') {
    p++;
    if (!isxdigit(*p))
      error_at(p, "invalid hex escape sequence");
    int c = 0;
    while (isxdigit(*p)) {
      c = (c << 4) + (isdigit(*p) ? *p - '0' :
                       islower(*p) ? *p - 'a' + 10 : *p - 'A' + 10);
      p++;
    }
    *new_pos = p;
    return c;
  }

  *new_pos = p + 1;

  switch (*p) {
  case 'a': return '\a';
  case 'b': return '\b';
  case 't': return '\t';
  case 'n': return '\n';
  case 'v': return '\v';
  case 'f': return '\f';
  case 'r': return '\r';
  // [GNU] \e for escape character
  case 'e': return 27;
  default: return *p;
  }
}

// Read a string literal and return a token
static Token *read_string_literal(char *start, char *quote) {
  char *end = quote + 1;
  while (*end != '"') {
    if (*end == '\n' || *end == '\0')
      error_at(start, "unclosed string literal");
    if (*end == '\\')
      end++;
    end++;
  }

  // Allocate buffer for the string contents
  char *buf = calloc_checked(1, end - quote);
  int len = 0;

  for (char *p = quote + 1; p < end;) {
    if (*p == '\\') {
      buf[len++] = read_escaped_char(&p, p + 1);
    } else {
      buf[len++] = *p++;
    }
  }

  Token *tok = new_token(TK_STR, start, end + 1);
  tok->ty = array_of(ty_char, len + 1);
  tok->str = buf;
  return tok;
}

// Read a UTF-8 string literal
static Token *read_utf8_string_literal(char *start, char *quote) {
  Token *tok = read_string_literal(start, quote);
  // If start has u8 prefix, type is still char array
  if (start != quote && (startswith(start, "u8")))
    tok->ty = array_of(ty_char, tok->ty->array_len);
  return tok;
}

// Read a character literal
static Token *read_char_literal(char *start, char *quote, Type *ty) {
  char *p = quote + 1;
  if (*p == '\0')
    error_at(start, "unclosed char literal");

  int c;
  if (*p == '\\')
    c = read_escaped_char(&p, p + 1);
  else
    c = decode_utf8(&p, p);

  char *end = strchr(p, '\'');
  if (!end)
    error_at(p, "unclosed char literal");

  Token *tok = new_token(TK_NUM, start, end + 1);
  tok->val = c;
  tok->ty = ty;
  return tok;
}

// Classify integer suffix
static bool is_int_suffix(char c) {
  return c == 'u' || c == 'U' || c == 'l' || c == 'L';
}

static Type *int_suffix_type(char *p, char **endp) {
  // Count u/U and l/L
  bool has_u = false;
  int num_l = 0;

  while (is_int_suffix(*p)) {
    if (*p == 'u' || *p == 'U') {
      has_u = true;
      p++;
    } else if (*p == 'l' || *p == 'L') {
      num_l++;
      p++;
    }
  }
  *endp = p;

  if (num_l >= 2) {
    return has_u ? ty_ulonglong : ty_longlong;
  } else if (num_l == 1) {
    return has_u ? ty_ulong : ty_long;
  } else {
    return has_u ? ty_uint : ty_int;
  }
}

// Read a number and return a token
static Token *read_number(char *start) {
  char *p = start;
  bool is_fp = false;

  // Read the number including hex, octal, binary prefixes
  // Preprocessing numbers can contain dots and exponents
  if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
    // Hex
    p += 2;
    while (isxdigit(*p) || *p == '.')
      p++;
    if (*p == 'p' || *p == 'P') {
      is_fp = true;
      p++;
      if (*p == '+' || *p == '-')
        p++;
      while (isdigit(*p))
        p++;
    }
  } else if (*p == '0' && (p[1] == 'b' || p[1] == 'B')) {
    // Binary
    p += 2;
    while (*p == '0' || *p == '1')
      p++;
  } else {
    // Decimal or octal
    while (isdigit(*p) || *p == '.')
      p++;
    if (*p == 'e' || *p == 'E') {
      is_fp = true;
      p++;
      if (*p == '+' || *p == '-')
        p++;
      while (isdigit(*p))
        p++;
    }
  }

  // Check if the number contains a dot or exponent → floating-point
  if (!is_fp) {
    bool is_hex = (start[0] == '0' && (start[1] == 'x' || start[1] == 'X'));
    for (char *q = start; q < p; q++) {
      if (*q == '.' || *q == 'p' || *q == 'P') { is_fp = true; break; }
      if (!is_hex && (*q == 'e' || *q == 'E')) { is_fp = true; break; }
    }
  }
  // Float/double suffix also indicates floating-point
  if (!is_fp && (*p == 'f' || *p == 'F') && !isalnum(p[1]))
    is_fp = true;

  if (is_fp) {
    // Floating-point
    Token *tok = new_token(TK_NUM, start, p);
    tok->fval = strtold(start, NULL);

    // Parse float/imaginary suffix in any order: f, F, l, L, i, fi, if, iL, Li, etc.
    Type *base_ty = ty_double;
    bool is_imag = false;
    for (int pass = 0; pass < 2; pass++) {
      if (*p == 'f' || *p == 'F') {
        base_ty = ty_float;
        p++;
      } else if (*p == 'l' || *p == 'L') {
        base_ty = ty_ldouble;
        p++;
      } else if (*p == 'i' || *p == 'I') {
        is_imag = true;
        p++;
      }
    }
    if (is_imag)
      tok->ty = complex_type(base_ty);
    else
      tok->ty = base_ty;
    tok->len = p - start;
    return tok;
  }

  // Integer
  Token *tok = new_token(TK_NUM, start, p);

  // Parse the integer value
  char *endptr;
  unsigned long long val;
  if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X'))
    val = strtoull(start, &endptr, 16);
  else if (start[0] == '0' && (start[1] == 'b' || start[1] == 'B'))
    val = strtoull(start + 2, &endptr, 2);
  else if (start[0] == '0')
    val = strtoull(start, &endptr, 8);
  else
    val = strtoull(start, &endptr, 10);

  tok->val = val;

  // Parse suffix
  Type *ty = int_suffix_type(endptr, &p);

  // Check for imaginary suffix on integer literal (e.g. 200i)
  bool int_imag = false;
  if (*p == 'i' || *p == 'I') {
    int_imag = true;
    p++;
  }

  // Promote to a wider type if needed
  if (ty == ty_int && val > INT_MAX)
    ty = ty_long;
  if (ty == ty_long && val > LONG_MAX)
    ty = ty_longlong;

  if (int_imag) {
    tok->fval = (long double)val;
    tok->ty = complex_type(ty);
  } else {
    tok->ty = ty;
  }
  tok->len = p - start;
  return tok;
}

// Convert keywords
static void convert_keywords(Token *tok) {
  for (Token *t = tok; t->kind != TK_EOF; t = t->next)
    if (t->kind == TK_IDENT && is_keyword(t))
      t->kind = TK_KEYWORD;
}

// Add line number info to tokens
static void add_line_numbers(Token *tok) {
  char *p = current_file->contents;
  int n = 1;

  do {
    if (p == tok->loc) {
      tok->line_no = n;
      tok = tok->next;
    }
    if (*p == '\n')
      n++;
  } while (*p++);
}

// Convert preprocessing number tokens to regular number tokens
void convert_pp_tokens(Token *tok) {
  for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_PP_NUM) {
      // Re-tokenize as a proper number
      File *saved = current_file;
      current_file = t->file;
      Token *num = read_number(t->loc);
      current_file = saved;

      t->kind = TK_NUM;
      t->val = num->val;
      t->fval = num->fval;
      t->ty = num->ty;
    }
  }
}

// Tokenize a string literal (for string concatenation in parser)
Token *tokenize_string_literal(Token *tok, Type *basety) {
  Token *t;
  if (basety->size == 1)
    t = read_string_literal(tok->loc, tok->loc);
  else
    t = read_string_literal(tok->loc, tok->loc);
  return t;
}

// Main tokenization function
Token *tokenize(File *file) {
  current_file = file;
  char *p = file->contents;

  Token head = {};
  Token *cur = &head;
  bool at_bol = true;
  bool has_space = false;

  while (*p) {
    // Skip line continuation (\<newline>)
    // Treat as whitespace for has_space: "no whitespace" check in #define
    // must see \<nl> as whitespace to distinguish object/function-like macros.
    if (startswith(p, "\\\n")) {
      p += 2;
      at_bol = false;
      has_space = true;
      continue;
    }

    // Skip line comments
    if (startswith(p, "//")) {
      p += 2;
      while (*p != '\n')
        p++;
      has_space = true;
      continue;
    }

    // Skip block comments
    if (startswith(p, "/*")) {
      char *q = strstr(p + 2, "*/");
      if (!q)
        error_at(p, "unclosed block comment");
      p = q + 2;
      has_space = true;
      continue;
    }

    // Skip whitespace
    if (*p == '\n') {
      p++;
      at_bol = true;
      has_space = false;
      continue;
    }

    if (isspace(*p)) {
      p++;
      has_space = true;
      continue;
    }

    // Numeric literal
    if (isdigit(*p) || (*p == '.' && isdigit(p[1]))) {
      // Read as preprocessing number for now
      char *start = p;
      while (isalnum(*p) || *p == '.' || *p == '_' ||
             ((*p == '+' || *p == '-') && (p[-1] == 'e' || p[-1] == 'E' ||
                                            p[-1] == 'p' || p[-1] == 'P')))
        p++;

      Token *tok = new_token(TK_PP_NUM, start, p);
      tok->at_bol = at_bol;
      tok->has_space = has_space;
      cur = cur->next = tok;
      at_bol = false;
      has_space = false;
      continue;
    }

    // String literal
    if (*p == '"') {
      Token *tok = read_string_literal(p, p);
      tok->at_bol = at_bol;
      tok->has_space = has_space;
      cur = cur->next = tok;
      p += tok->len;
      at_bol = false;
      has_space = false;
      continue;
    }

    // UTF-8 string literal
    if (startswith(p, "u8\"")) {
      Token *tok = read_utf8_string_literal(p, p + 2);
      tok->at_bol = at_bol;
      tok->has_space = has_space;
      cur = cur->next = tok;
      p += tok->len;
      at_bol = false;
      has_space = false;
      continue;
    }

    // Wide/UTF-16/UTF-32 string literal
    if ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') && p[1] == '"') {
      // For simplicity, read as a regular string literal
      Token *tok = read_string_literal(p, p + 1);
      tok->at_bol = at_bol;
      tok->has_space = has_space;
      cur = cur->next = tok;
      p += tok->len;
      at_bol = false;
      has_space = false;
      continue;
    }

    // Character literal
    if (*p == '\'') {
      Token *tok = read_char_literal(p, p, ty_int);
      tok->at_bol = at_bol;
      tok->has_space = has_space;
      cur = cur->next = tok;
      p += tok->len;
      at_bol = false;
      has_space = false;
      continue;
    }

    // Wide/UTF character literal
    if ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') && p[1] == '\'') {
      Type *ty = (p[0] == 'L') ? ty_int :
                 (p[0] == 'u') ? ty_ushort : ty_uint;
      Token *tok = read_char_literal(p, p + 1, ty);
      tok->at_bol = at_bol;
      tok->has_space = has_space;
      cur = cur->next = tok;
      p += tok->len;
      at_bol = false;
      has_space = false;
      continue;
    }

    // Identifier or keyword
    int ident_len = read_ident(p);
    if (ident_len) {
      Token *tok = new_token(TK_IDENT, p, p + ident_len);
      tok->at_bol = at_bol;
      tok->has_space = has_space;
      cur = cur->next = tok;
      p += ident_len;
      at_bol = false;
      has_space = false;
      continue;
    }

    // Punctuator
    int punct_len = read_punct(p);
    if (punct_len) {
      Token *tok = new_token(TK_PUNCT, p, p + punct_len);
      tok->at_bol = at_bol;
      tok->has_space = has_space;
      cur = cur->next = tok;
      p += punct_len;
      at_bol = false;
      has_space = false;
      continue;
    }

    error_at(p, "invalid token");
  }

  Token *eof = new_token(TK_EOF, p, p);
  eof->at_bol = at_bol;
  eof->has_space = has_space;
  cur->next = eof;

  add_line_numbers(head.next);
  convert_keywords(head.next);
  return head.next;
}

// Read a file and return its contents
static char *read_file(char *path) {
  FILE *fp;

  if (strcmp(path, "-") == 0) {
    // Read from stdin
    fp = stdin;
  } else {
    fp = fopen(path, "r");
    if (!fp)
      return NULL;
  }

  char *buf;
  size_t buflen;
  FILE *out = open_memstream(&buf, &buflen);

  for (;;) {
    char buf2[4096];
    int n = fread(buf2, 1, sizeof(buf2), fp);
    if (n == 0)
      break;
    fwrite(buf2, 1, n, out);
  }

  if (fp != stdin)
    fclose(fp);

  // Make sure the file ends with a newline
  fflush(out);
  if (buflen == 0 || buf[buflen - 1] != '\n')
    fputc('\n', out);
  fputc('\0', out);
  fclose(out);
  return buf;
}

File *new_file(char *name, int file_no, char *contents) {
  File *file = calloc_checked(1, sizeof(File));
  file->name = name;
  file->display_name = name;
  file->file_no = file_no;
  file->contents = contents;
  return file;
}

// Tokenize a file
Token *tokenize_file(char *path) {
  char *p = read_file(path);
  if (!p)
    return NULL;

  // Track input files
  static int file_no = 0;
  File *file = new_file(path, file_no + 1, p);

  // Store in input_files array
  input_files = realloc(input_files, sizeof(File *) * (num_input_files + 2));
  input_files[num_input_files++] = file;
  input_files[num_input_files] = NULL;
  file_no++;

  return tokenize(file);
}
