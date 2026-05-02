// tokenize_v2.c — Phase 1 swap-out candidate.
//
// Specified by docs/specs/01_tokenizer.md (commit 01f6ee4).
// Written without reference to src/tokenize.c — only the function
// signatures in src/cc.h and the spec are inputs.
//
// Validation: must produce byte-identical -fdump-tokens output to
// the existing src/tokenize.c on the corpus driven by
// scripts/validate_tokenizer.sh.

#include "cc.h"

// ---------------------------------------------------------------------------
// Section 11/12: error reporting. Static "current_file" is module-level so
// error_at can find the source line containing a free `loc` pointer.
// ---------------------------------------------------------------------------

static File *current_file_v2;

static void verror_at_v2(int line_no, const char *loc, const char *fmt,
                          va_list ap) {
    const char *input = current_file_v2->contents;
    const char *line = loc;
    while (input < line && line[-1] != '\n') line--;
    const char *end = loc;
    while (*end && *end != '\n') end++;

    int indent = fprintf(stderr, "%s:%d: ",
                         current_file_v2->display_name, line_no);
    fprintf(stderr, "%.*s\n", (int)(end - line), line);

    int pos = (int)(loc - line) + indent;
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
    for (const char *p = current_file_v2->contents; p < loc; p++)
        if (*p == '\n') line_no++;
    va_list ap;
    va_start(ap, fmt);
    verror_at_v2(line_no, loc, fmt, ap);
    va_end(ap);
    exit(1);
}

// Walk the macro-origin chain back to the user-source token.
static Token *macro_use_site_v2(Token *tok) {
    while (tok && tok->origin) tok = tok->origin;
    return tok;
}

_Noreturn void error_tok(Token *tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Token *src = macro_use_site_v2(tok);
    if (src && src->file) {
        File *saved = current_file_v2;
        current_file_v2 = src->file;
        verror_at_v2(src->line_no, src->loc, fmt, ap);
        current_file_v2 = saved;
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
    Token *src = macro_use_site_v2(tok);
    if (src && src->file) {
        File *saved = current_file_v2;
        current_file_v2 = src->file;
        verror_at_v2(src->line_no, src->loc, fmt, ap);
        current_file_v2 = saved;
    } else {
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
    va_end(ap);
}

// ---------------------------------------------------------------------------
// Token operations (cc.h-exported).
// ---------------------------------------------------------------------------

bool equal(Token *tok, const char *op) {
    size_t n = strlen(op);
    return tok->len == (int)n && !memcmp(tok->loc, op, n);
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

// ---------------------------------------------------------------------------
// Input file tracking.
// ---------------------------------------------------------------------------

static File **input_files_v2;
static int num_input_files_v2;

File **get_input_files(void) { return input_files_v2; }
File *get_file(Token *tok) { return tok->file; }

File *new_file(char *name, int file_no, char *contents) {
    File *f = calloc_checked(1, sizeof(File));
    f->name = name;
    f->display_name = name;
    f->file_no = file_no;
    f->contents = contents;
    return f;
}

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

static Token *new_token(TokenKind kind, char *start, char *end) {
    Token *tok = calloc_checked(1, sizeof(Token));
    tok->kind = kind;
    tok->loc = start;
    tok->len = (int)(end - start);
    tok->file = current_file_v2;
    return tok;
}

static bool starts_with(const char *p, const char *q) {
    return strncmp(p, q, strlen(q)) == 0;
}

// ---------------------------------------------------------------------------
// Section 5: identifiers (UTF-8 via decode_utf8 + is_ident{1,2}).
// Returns 0 if `start` is not a valid ident-start; otherwise byte length.
// ---------------------------------------------------------------------------

static int read_ident_v2(char *start) {
    char *p = start;
    uint32_t c = decode_utf8(&p, start);
    if (!is_ident1(c)) return 0;
    for (;;) {
        if (!*p) break;
        char *q;
        c = decode_utf8(&q, p);
        if (!is_ident2(c)) break;
        p = q;
    }
    return (int)(p - start);
}

// ---------------------------------------------------------------------------
// Section 9: punctuators (3/2/1-byte maximal munch).
// ---------------------------------------------------------------------------

static int read_punct_v2(char *p) {
    static const char *p3[] = { "<<=", ">>=", "...", NULL };
    for (int i = 0; p3[i]; i++)
        if (starts_with(p, p3[i])) return 3;
    static const char *p2[] = {
        "==", "!=", "<=", ">=", "->", "+=", "-=", "*=", "/=",
        "%=", "&=", "|=", "^=", "&&", "||", "<<", ">>", "++",
        "--", "##", NULL,
    };
    for (int i = 0; p2[i]; i++)
        if (starts_with(p, p2[i])) return 2;
    return ispunct((unsigned char)*p) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Section 10: keywords (53 entries).
// ---------------------------------------------------------------------------

static bool is_keyword_v2(Token *tok) {
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
        NULL,
    };
    for (int i = 0; kw[i]; i++)
        if (equal(tok, kw[i])) return true;
    return false;
}

static void convert_keywords_v2(Token *head) {
    for (Token *t = head; t->kind != TK_EOF; t = t->next)
        if (t->kind == TK_IDENT && is_keyword_v2(t))
            t->kind = TK_KEYWORD;
}

// ---------------------------------------------------------------------------
// Section 7.1: \-escape decoding inside string and char literals.
// On entry `*p` is the byte AFTER the backslash. Returns the resulting
// integer code; advances `*new_pos` past the consumed bytes.
// ---------------------------------------------------------------------------

static int read_escaped_char_v2(char **new_pos, char *p) {
    // Octal: 1-3 digits.
    if ('0' <= *p && *p <= '7') {
        int c = *p++ - '0';
        if ('0' <= *p && *p <= '7') c = (c << 3) + (*p++ - '0');
        if ('0' <= *p && *p <= '7') c = (c << 3) + (*p++ - '0');
        *new_pos = p;
        return c;
    }
    // Hex: \xNN... (continues while hex digits present).
    if (*p == 'x') {
        p++;
        if (!isxdigit((unsigned char)*p))
            error_at(p, "invalid hex escape sequence");
        int c = 0;
        while (isxdigit((unsigned char)*p)) {
            int d;
            if (isdigit((unsigned char)*p))      d = *p - '0';
            else if (islower((unsigned char)*p)) d = *p - 'a' + 10;
            else                                 d = *p - 'A' + 10;
            c = (c << 4) + d;
            p++;
        }
        *new_pos = p;
        return c;
    }
    // Named escapes; default = the byte verbatim (covers \\, \", \', \?).
    *new_pos = p + 1;
    switch (*p) {
        case 'a': return 7;
        case 'b': return 8;
        case 't': return 9;
        case 'n': return 10;
        case 'v': return 11;
        case 'f': return 12;
        case 'r': return 13;
        case 'e': return 27;   // GNU \e
        default:  return (unsigned char)*p;
    }
}

// ---------------------------------------------------------------------------
// Section 7: string literals. `start` is the first byte of the source
// span (so `L"…"` has start ahead of quote); `quote` is the opening `"`.
// Token's source span runs [start, end+1) where end is the closing `"`.
// ---------------------------------------------------------------------------

static char *find_string_end(char *start, char *quote) {
    char *end = quote + 1;
    while (*end != '"') {
        if (*end == '\n' || *end == '\0')
            error_at(start, "unclosed string literal");
        if (*end == '\\') end++;
        end++;
    }
    return end;
}

static Token *read_string_literal_v2(char *start, char *quote) {
    char *end = find_string_end(start, quote);
    char *buf = calloc_checked(1, (size_t)(end - quote));
    int n = 0;
    for (char *p = quote + 1; p < end; ) {
        if (*p == '\\') {
            buf[n++] = (char)read_escaped_char_v2(&p, p + 1);
        } else {
            buf[n++] = *p++;
        }
    }
    Token *tok = new_token(TK_STR, start, end + 1);
    tok->ty = array_of(ty_char, n + 1);
    tok->str = buf;
    return tok;
}

// Spec §7: u8/L/u/U prefixes are accepted but the resulting array
// element type is char in all cases (known divergence; spec §13).
static Token *read_utf8_or_wide_string_v2(char *start, char *quote) {
    Token *tok = read_string_literal_v2(start, quote);
    return tok;
}

// ---------------------------------------------------------------------------
// Section 8: character literals.
// ---------------------------------------------------------------------------

static Token *read_char_literal_v2(char *start, char *quote, Type *ty) {
    char *p = quote + 1;
    if (*p == '\0') error_at(start, "unclosed char literal");
    int c;
    if (*p == '\\') {
        c = read_escaped_char_v2(&p, p + 1);
    } else {
        // Single codepoint via UTF-8.
        c = (int)decode_utf8(&p, p);
    }
    char *end = strchr(p, '\'');
    if (!end) error_at(p, "unclosed char literal");
    Token *tok = new_token(TK_NUM, start, end + 1);
    tok->val = c;
    tok->ty = ty;
    return tok;
}

// ---------------------------------------------------------------------------
// Section 6.1: preprocessing-number boundary scan.
// Starts at digit OR `.<digit>`. Advances over [0-9A-Za-z_.] plus
// `+`/`-` immediately following one of e E p P (exponent sign).
// Returns one past the last consumed byte.
// ---------------------------------------------------------------------------

static char *scan_pp_num_v2(char *start) {
    char *p = start;
    for (;;) {
        if (isalnum((unsigned char)*p) || *p == '.' || *p == '_') {
            p++; continue;
        }
        if ((*p == '+' || *p == '-') && p > start &&
            (p[-1] == 'e' || p[-1] == 'E' || p[-1] == 'p' || p[-1] == 'P')) {
            p++; continue;
        }
        break;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Section 6.2-6.3: integer-suffix classification + value/type lowering.
// ---------------------------------------------------------------------------

static Type *int_suffix_type_v2(char *p, char **endp) {
    bool has_u = false;
    int num_l = 0;
    while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') {
        if (*p == 'u' || *p == 'U') has_u = true;
        else                         num_l++;
        p++;
    }
    *endp = p;
    if (num_l >= 2) return has_u ? ty_ulonglong : ty_longlong;
    if (num_l == 1) return has_u ? ty_ulong     : ty_long;
    return                  has_u ? ty_uint      : ty_int;
}

static Token *lower_pp_num_v2(char *start) {
    // Determine FP vs integer per spec §6.2.
    bool is_fp = false;
    char *p = start;
    bool is_hex_prefix =
        (start[0] == '0' && (start[1] == 'x' || start[1] == 'X'));

    // Walk the literal once to find the suffix start (`p` will point
    // there afterwards) and decide is_fp.
    if (is_hex_prefix) {
        p += 2;
        while (isxdigit((unsigned char)*p) || *p == '.') {
            if (*p == '.') is_fp = true;
            p++;
        }
        if (*p == 'p' || *p == 'P') {
            is_fp = true;
            p++;
            if (*p == '+' || *p == '-') p++;
            while (isdigit((unsigned char)*p)) p++;
        }
    } else if (start[0] == '0' && (start[1] == 'b' || start[1] == 'B')) {
        p += 2;
        while (*p == '0' || *p == '1') p++;
    } else {
        while (isdigit((unsigned char)*p) || *p == '.') {
            if (*p == '.') is_fp = true;
            p++;
        }
        if (*p == 'e' || *p == 'E') {
            is_fp = true;
            p++;
            if (*p == '+' || *p == '-') p++;
            while (isdigit((unsigned char)*p)) p++;
        }
    }
    // Float suffix (f/F) immediately after digits/exponent => floating-point.
    if (!is_fp && (*p == 'f' || *p == 'F') &&
        !isalnum((unsigned char)p[1]) && p[1] != '_')
        is_fp = true;

    if (is_fp) {
        // Floating-point parse.
        Token *tok = new_token(TK_NUM, start, p);
        tok->fval = strtold(start, NULL);
        Type *base = ty_double;
        bool is_imag = false;
        // Two passes to allow suffixes in either order (e.g. `fi` or `if`).
        for (int pass = 0; pass < 2; pass++) {
            if (*p == 'f' || *p == 'F') { base = ty_float;   p++; }
            else if (*p == 'l' || *p == 'L') { base = ty_ldouble; p++; }
            else if (*p == 'i' || *p == 'I') { is_imag = true; p++; }
        }
        tok->ty = is_imag ? complex_type(base) : base;
        tok->len = (int)(p - start);
        return tok;
    }

    // Integer parse.
    Token *tok = new_token(TK_NUM, start, p);
    char *endptr;
    unsigned long long val;
    if (is_hex_prefix)
        val = strtoull(start, &endptr, 16);
    else if (start[0] == '0' && (start[1] == 'b' || start[1] == 'B'))
        val = strtoull(start + 2, &endptr, 2);
    else if (start[0] == '0')
        val = strtoull(start, &endptr, 8);
    else
        val = strtoull(start, &endptr, 10);

    tok->val = (int64_t)val;
    Type *ty = int_suffix_type_v2(endptr, &p);

    bool int_imag = false;
    if (*p == 'i' || *p == 'I') { int_imag = true; p++; }

    if (ty == ty_int  && val > (unsigned long long)INT_MAX)  ty = ty_long;
    if (ty == ty_long && val > (unsigned long long)LONG_MAX) ty = ty_longlong;

    if (int_imag) {
        tok->fval = (long double)val;
        tok->ty   = complex_type(ty);
    } else {
        tok->ty = ty;
    }
    tok->len = (int)(p - start);
    return tok;
}

// Walk the post-tokenize list and lower TK_PP_NUM to TK_NUM. Matches the
// existing convert_pp_tokens semantics exactly: re-tokenize numerically
// using the same `current_file_v2` so error messages still attribute
// correctly.
void convert_pp_tokens(Token *head) {
    File *saved = current_file_v2;
    for (Token *t = head; t && t->kind != TK_EOF; t = t->next) {
        if (t->kind != TK_PP_NUM) continue;
        if (t->file) current_file_v2 = t->file;
        Token *n = lower_pp_num_v2(t->loc);
        t->kind = TK_NUM;
        t->val = n->val;
        t->fval = n->fval;
        t->ty = n->ty;
    }
    current_file_v2 = saved;
}

// ---------------------------------------------------------------------------
// Section 11: post-tokenize line-number assignment.
// ---------------------------------------------------------------------------

static void add_line_numbers_v2(Token *head) {
    char *p = current_file_v2->contents;
    int line = 1;
    Token *t = head;
    do {
        if (t && p == t->loc) { t->line_no = line; t = t->next; }
        if (*p == '\n') line++;
    } while (*p++);
}

// ---------------------------------------------------------------------------
// Section 4: the main dispatch loop.
// ---------------------------------------------------------------------------

Token *tokenize(File *file) {
    current_file_v2 = file;
    char *p = file->contents;
    Token head = (Token){0};
    Token *cur = &head;
    bool at_bol = true;
    bool has_space = false;

    while (*p) {
        // 1. Line continuation: \<newline>. Treated as whitespace.
        if (p[0] == '\\' && p[1] == '\n') {
            p += 2;
            has_space = true;
            continue;
        }
        // 2. Line comment.
        if (p[0] == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') p++;
            has_space = true;
            continue;
        }
        // 3. Block comment.
        if (p[0] == '/' && p[1] == '*') {
            char *q = strstr(p + 2, "*/");
            if (!q) error_at(p, "unclosed block comment");
            p = q + 2;
            has_space = true;
            continue;
        }
        // 4. Newline.
        if (*p == '\n') {
            p++;
            at_bol = true;
            has_space = false;
            continue;
        }
        // 5. Other whitespace.
        if (isspace((unsigned char)*p)) {
            p++;
            has_space = true;
            continue;
        }

        Token *tok = NULL;

        // 6. Numeric literal: digit or .<digit>. Read as preprocessing
        //    number (TK_PP_NUM); convert_pp_tokens lowers it later.
        if (isdigit((unsigned char)*p) ||
            (*p == '.' && isdigit((unsigned char)p[1]))) {
            char *start = p;
            p = scan_pp_num_v2(p);
            tok = new_token(TK_PP_NUM, start, p);
        }
        // 7. Plain string literal.
        else if (*p == '"') {
            tok = read_string_literal_v2(p, p);
            p += tok->len;
        }
        // 8. UTF-8 string literal.
        else if (p[0] == 'u' && p[1] == '8' && p[2] == '"') {
            tok = read_utf8_or_wide_string_v2(p, p + 2);
            p += tok->len;
        }
        // 9. Wide / UTF-16 / UTF-32 string literal.
        else if ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') &&
                 p[1] == '"') {
            tok = read_utf8_or_wide_string_v2(p, p + 1);
            p += tok->len;
        }
        // 10. Plain character literal.
        else if (*p == '\'') {
            tok = read_char_literal_v2(p, p, ty_int);
            p += tok->len;
        }
        // 11. Wide / UTF-16 / UTF-32 character literal.
        else if ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') &&
                 p[1] == '\'') {
            Type *ty = (p[0] == 'L') ? ty_int
                     : (p[0] == 'u') ? ty_ushort : ty_uint;
            tok = read_char_literal_v2(p, p + 1, ty);
            p += tok->len;
        } else {
            // 12. Identifier or keyword.
            int n = read_ident_v2(p);
            if (n) {
                tok = new_token(TK_IDENT, p, p + n);
                p += n;
            } else {
                // 13. Punctuator.
                int m = read_punct_v2(p);
                if (m) {
                    tok = new_token(TK_PUNCT, p, p + m);
                    p += m;
                } else {
                    // 14. None of the above.
                    error_at(p, "invalid token");
                }
            }
        }

        tok->at_bol = at_bol;
        tok->has_space = has_space;
        cur = cur->next = tok;
        at_bol = false;
        has_space = false;
    }

    Token *eof = new_token(TK_EOF, p, p);
    eof->at_bol = at_bol;
    eof->has_space = has_space;
    cur->next = eof;

    add_line_numbers_v2(head.next);
    convert_keywords_v2(head.next);
    return head.next;
}

// ---------------------------------------------------------------------------
// File I/O (cc.h-exported).
// ---------------------------------------------------------------------------

static char *read_file_v2(const char *path) {
    FILE *fp = (strcmp(path, "-") == 0) ? stdin : fopen(path, "r");
    if (!fp) return NULL;

    char *buf;
    size_t buflen;
    FILE *out = open_memstream(&buf, &buflen);

    char chunk[4096];
    for (;;) {
        size_t n = fread(chunk, 1, sizeof chunk, fp);
        if (!n) break;
        fwrite(chunk, 1, n, out);
    }
    if (fp != stdin) fclose(fp);
    fflush(out);
    if (buflen == 0 || buf[buflen - 1] != '\n') fputc('\n', out);
    fputc('\0', out);
    fclose(out);
    return buf;
}

Token *tokenize_file(char *path) {
    char *content = read_file_v2(path);
    if (!content) return NULL;

    static int file_no_v2 = 0;
    File *file = new_file(path, file_no_v2 + 1, content);

    input_files_v2 = realloc(
        input_files_v2, sizeof(File *) * (size_t)(num_input_files_v2 + 2));
    input_files_v2[num_input_files_v2++] = file;
    input_files_v2[num_input_files_v2] = NULL;
    file_no_v2++;

    return tokenize(file);
}

// ---------------------------------------------------------------------------
// Re-tokenize a string literal (from a peer to tokenize_file). Used by the
// preprocessor's _Pragma() handling and similar. Tokenizes the contents
// using a synthetic File, then assigns the requested base type.
// ---------------------------------------------------------------------------

Token *tokenize_string_literal(Token *tok, Type *basety) {
    File *file = new_file(current_file_v2 ? current_file_v2->name : "(string)",
                          tok->file ? tok->file->file_no : 0,
                          tok->str);
    Token *out = tokenize(file);
    // Override array element type to the requested base (for L"…" etc.).
    for (Token *t = out; t->kind != TK_EOF; t = t->next) {
        if (t->kind == TK_STR) {
            t->ty = array_of(basety, t->ty->array_len);
        }
    }
    return out;
}
