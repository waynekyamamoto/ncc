// preprocess_v2.c — Phase 2 spec-derived C preprocessor.
//
// Implementation derived from docs/specs/02_preprocessor.md under
// strict no-peek discipline (no consultation of the chibicc-lineage
// src/preprocess.c during impl).  Built into the alternate `ncc-v2`
// binary; validated against the canonical `ncc` via
// scripts/validate_preprocessor.sh.

#include "cc.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

//
// 2. Data model (spec §2)
//

struct Hideset {
    Hideset *next;
    char *name;
};

typedef struct MacroParam MacroParam;
struct MacroParam {
    MacroParam *next;
    char *name;
};

typedef struct MacroArg MacroArg;
struct MacroArg {
    MacroArg *next;
    char *name;
    bool is_va_args;
    Token *tok;       // raw, not pre-expanded
    Token *expanded;  // after one round of preprocess2
};

typedef Token *MacroHandlerFn(Token *tmpl);

typedef struct Macro Macro;
struct Macro {
    char *name;
    bool is_objlike;
    MacroParam *params;
    char *va_args_name;
    Token *body;
    MacroHandlerFn *handler;
};

typedef struct CondIncl CondIncl;
struct CondIncl {
    CondIncl *next;
    enum { IN_THEN, IN_ELIF, IN_ELSE } ctx;
    Token *tok;
    bool included;
};

//
// 3. Module state (spec §3)
//

static HashMap macros;            // macro table
static CondIncl *cond_incl;       // conditional-inclusion stack
StringArray include_paths;        // declared extern in cc.h
static PragmaHandler *pragma_handler;
static int counter_val;

// __BASE_FILE__ source — defined in main.c (the driver).
extern char *base_file;

// Public counter, exposed for performance reporting from the driver.
long pp_token_count;

//
// Forward declarations (only what later chunks need to reference)
//

static Token *preprocess2(Token *tok);

//
// Public-API hooks
//

void set_pragma_handler(PragmaHandler *fn) {
    pragma_handler = fn;
}

void add_include_path(char *path) {
    strarray_push(&include_paths, path);
}

//
// Utility helpers (spec §2.1, §5.1)
//

// `#`-at-line-start detection for directive dispatch.
static bool is_hash(Token *tok) {
    return tok->at_bol && equal(tok, "#");
}

// Advance past the rest of the current logical line.
static Token *skip_line(Token *tok) {
    if (tok->at_bol)
        return tok;
    warn_tok(tok, "extra token");
    while (!tok->at_bol)
        tok = tok->next;
    return tok;
}

// Deep-copy one Token.  Sets next = NULL.
static Token *copy_token(Token *tok) {
    Token *t = calloc_checked(1, sizeof(Token));
    *t = *tok;
    t->next = NULL;
    return t;
}

// Copy `tok` into a fresh Token, set kind=TK_EOF, len=0.
static Token *new_eof(Token *tok) {
    Token *t = copy_token(tok);
    t->kind = TK_EOF;
    t->len = 0;
    return t;
}

// Copy tok1 (stopping at TK_EOF) and link the copy chain to tok2.
static Token *append(Token *tok1, Token *tok2) {
    Token head = {0};
    Token *cur = &head;
    for (; tok1->kind != TK_EOF; tok1 = tok1->next)
        cur = cur->next = copy_token(tok1);
    cur->next = tok2;
    return head.next;
}

// Copy tokens from tok up to (but not including) the next at_bol
// token; terminate with a TK_EOF sentinel.
static Token *copy_line(Token **rest, Token *tok) {
    Token head = {0};
    Token *cur = &head;
    for (; !tok->at_bol; tok = tok->next)
        cur = cur->next = copy_token(tok);
    cur->next = new_eof(tok);
    *rest = tok;
    return head.next;
}

// Deep-copy a token list to (and including) its TK_EOF.
static Token *copy_token_list(Token *tok) {
    Token head = {0};
    Token *cur = &head;
    for (; tok->kind != TK_EOF; tok = tok->next)
        cur = cur->next = copy_token(tok);
    cur->next = new_eof(tok);
    return head.next;
}

//
// Public top-level entry (spec §4) — preprocess2 is a pass-through
// stub until Chunk 7; this lets the dual-build link cleanly with the
// expected behavior shape.
//

Token *preprocess(Token *tok) {
    pp_token_count = 0;
    tok = preprocess2(tok);
    if (cond_incl)
        error_tok(cond_incl->tok, "unterminated conditional directive");
    convert_pp_tokens(tok);
    return tok;
}

static Token *preprocess2(Token *tok) {
    // TODO Chunk 7: directive dispatch + macro expansion per spec §5.
    return tok;
}

//
// Hideset operations (spec §2.4)
//

static Hideset *new_hideset(char *name) {
    Hideset *hs = calloc_checked(1, sizeof(Hideset));
    hs->name = name;
    return hs;
}

// Set union — concatenate hs1 onto a chain shared with hs2's tail.
// No deduplication: harmless because hideset_contains returns on
// first match (spec §2.4 Q3).
static Hideset *hideset_union(Hideset *hs1, Hideset *hs2) {
    Hideset head = {0};
    Hideset *cur = &head;
    for (; hs1; hs1 = hs1->next)
        cur = cur->next = new_hideset(hs1->name);
    cur->next = hs2;
    return head.next;
}

// Set intersection — elements of hs1 whose name also appears in hs2.
// Used by expand_macro for the painter's-rule (spec §6.2 / Q9) when
// expanding function-like macros.
static Hideset *hideset_intersection(Hideset *hs1, Hideset *hs2) {
    Hideset head = {0};
    Hideset *cur = &head;
    for (; hs1; hs1 = hs1->next) {
        for (Hideset *h = hs2; h; h = h->next) {
            if (!strcmp(hs1->name, h->name)) {
                cur = cur->next = new_hideset(hs1->name);
                break;
            }
        }
    }
    return head.next;
}

// Membership test — linear scan with strncmp.
static bool hideset_contains(Hideset *hs, char *s, int len) {
    for (; hs; hs = hs->next)
        if ((int)strlen(hs->name) == len && !strncmp(hs->name, s, len))
            return true;
    return false;
}

// Copy every token in `tok` (up to TK_EOF), unioning `hs` into each
// copy's hideset field; copies the EOF too so the result is an
// independent list.
static Token *add_hideset(Token *tok, Hideset *hs) {
    Token head = {0};
    Token *cur = &head;
    for (; tok->kind != TK_EOF; tok = tok->next) {
        Token *t = copy_token(tok);
        t->hideset = hideset_union(t->hideset, hs);
        cur = cur->next = t;
    }
    cur->next = new_eof(tok);
    return head.next;
}

//
// Macro table (spec §2.6)
//

static Macro *find_macro(Token *tok) {
    if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD)
        return NULL;
    return hashmap_get2(&macros, tok->loc, tok->len);
}

static Macro *add_macro(char *name, bool is_objlike, Token *body) {
    Macro *m = calloc_checked(1, sizeof(Macro));
    m->name = name;
    m->is_objlike = is_objlike;
    m->body = body;
    hashmap_put(&macros, name, m);
    return m;
}

void undef_macro(char *name) {
    hashmap_delete(&macros, name);
}

//
// Builtin handler macros (spec §6.5)
//

// Walk the expansion-origin chain to the deepest source token (the
// one the user actually wrote).  Used by file_macro and line_macro
// so that __FILE__ / __LINE__ report the macro use site rather than
// any internal expansion site.
static Token *origin_walk(Token *tok) {
    while (tok->origin)
        tok = tok->origin;
    return tok;
}

static Token *file_macro(Token *tmpl) {
    Token *t = origin_walk(tmpl);
    char *buf = format("\"%s\"", t->file->display_name);
    return tokenize(new_file(t->file->name, t->file->file_no, buf));
}

static Token *line_macro(Token *tmpl) {
    Token *t = origin_walk(tmpl);
    int line = t->line_no + t->file->line_delta;
    char *buf = format("%d", line);
    return tokenize(new_file(t->file->name, t->file->file_no, buf));
}

static Token *counter_macro(Token *tmpl) {
    char *buf = format("%d", counter_val++);
    return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

static Token *timestamp_macro(Token *tmpl) {
    return tokenize(new_file(tmpl->file->name, tmpl->file->file_no,
                             "\"Unknown\""));
}

static Token *base_file_macro(Token *tmpl) {
    char *buf = format("\"%s\"", base_file ? base_file : "");
    return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

//
// define_macro — register a macro from a (name, body_text) pair.
// Object-like form: name is just the macro name.
// Function-like form: name has the shape "FOO(p1,p2,...)" or
// "FOO(p1,p2,name...)" or "FOO(...)" for variadic; the parameter
// list is parsed from the parenthesised tail.  (Spec §6.1; called
// by init_macros and by the driver for -D flags.)
//

void define_macro(char *name, char *buf) {
    Token *body = tokenize(new_file("<built-in>", 1, buf));

    char *paren = strchr(name, '(');
    if (!paren) {
        add_macro(name, true, body);
        return;
    }

    // Function-like: split the name and parse parameters.
    char *bare = strndup_checked(name, paren - name);
    Macro *m = add_macro(bare, false, body);

    // Tokenize "(p1,p2,...)" so we can reuse the lexer's identifier
    // and "..."  recognition rather than hand-rolling a string parser.
    Token *p = tokenize(new_file("<built-in>", 1, paren));
    p = skip(p, "(");

    MacroParam head = {0};
    MacroParam *cur = &head;
    char *va_args_name = NULL;
    while (p->kind != TK_EOF && !equal(p, ")")) {
        if (cur != &head)
            p = skip(p, ",");

        if (equal(p, "...")) {
            va_args_name = "__VA_ARGS__";
            p = p->next;
            break;
        }

        if (p->kind != TK_IDENT && p->kind != TK_KEYWORD)
            error("define_macro: expected parameter name in '%s'", name);

        // Named-variadic form: "name..." (GNU extension).
        if (equal(p->next, "...")) {
            va_args_name = strndup_checked(p->loc, p->len);
            p = p->next->next;
            break;
        }

        MacroParam *param = calloc_checked(1, sizeof(MacroParam));
        param->name = strndup_checked(p->loc, p->len);
        cur = cur->next = param;
        p = p->next;
    }
    m->params = head.next;
    m->va_args_name = va_args_name;
}

//
// init_macros — register all predefined macros for the macOS / AArch64
// target (spec §11).  Per Q1, every macro is listed exactly once
// (notably __SCHAR_MAX__).
//

void init_macros(void) {
    // §11.1 Standard C predefines
    define_macro("__STDC__", "1");
    define_macro("__STDC_VERSION__", "201112L");
    define_macro("__STDC_HOSTED__", "1");
    define_macro("__STDC_NO_ATOMICS__", "1");
    define_macro("__STDC_NO_COMPLEX__", "1");
    define_macro("__STDC_NO_THREADS__", "1");
    define_macro("__STDC_NO_VLA__", "1");
    define_macro("__STDC_UTF_16__", "1");
    define_macro("__STDC_UTF_32__", "1");

    // §11.2 Sizeof / integer-type predefines (LP64 macOS aarch64)
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

    // §11.3 Limit predefines (Q1: __SCHAR_MAX__ once)
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

    // §11.4 Float / double / long-double predefines
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

    // §11.5 Byte-order predefines
    define_macro("__ORDER_LITTLE_ENDIAN__", "1234");
    define_macro("__ORDER_BIG_ENDIAN__", "4321");
    define_macro("__BYTE_ORDER__", "1234");
    define_macro("__LITTLE_ENDIAN__", "1");

    // §11.6 ARM64 / Apple platform
    define_macro("__aarch64__", "1");
    define_macro("__arm64__", "1");
    define_macro("__arm64", "1");
    define_macro("__AARCH64EL__", "1");
    define_macro("__APPLE__", "1");
    define_macro("__MACH__", "1");
    define_macro("__DARWIN_C_LEVEL", "900000L");

    // §11.7 Apple TargetConditionals.h predefines
    define_macro("TARGET_OS_MAC", "1");
    define_macro("TARGET_OS_OSX", "1");
    define_macro("TARGET_OS_IPHONE", "0");
    define_macro("TARGET_OS_IOS", "0");
    define_macro("TARGET_OS_WATCH", "0");
    define_macro("TARGET_OS_TV", "0");
    define_macro("TARGET_OS_SIMULATOR", "0");
    define_macro("TARGET_OS_EMBEDDED", "0");
    define_macro("TARGET_OS_MACCATALYST", "0");
    define_macro("TARGET_OS_DRIVERKIT", "0");
    define_macro("TARGET_CPU_ARM64", "1");
    define_macro("TARGET_CPU_ARM", "0");
    define_macro("TARGET_CPU_X86", "0");
    define_macro("TARGET_CPU_X86_64", "0");
    define_macro("TARGET_RT_LITTLE_ENDIAN", "1");
    define_macro("TARGET_RT_BIG_ENDIAN", "0");
    define_macro("TARGET_RT_64_BIT", "1");
    define_macro("TARGET_RT_MAC_MACHO", "1");

    // §11.8 Darwin deployment-target predefines (macOS 14 baseline)
    define_macro("__MAC_OS_X_VERSION_MIN_REQUIRED", "140000");
    define_macro("__MAC_OS_X_VERSION_MAX_ALLOWED", "140000");
    define_macro("__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__", "140000");
    define_macro("__ENVIRONMENT_OS_VERSION_MIN_REQUIRED__", "140000");

    // §11.9 GCC compatibility (advertise GCC 12)
    define_macro("__GNUC__", "12");
    define_macro("__GNUC_MINOR__", "1");
    define_macro("__GNUC_PATCHLEVEL__", "0");
    define_macro("__GNUC_STDC_INLINE__", "1");
    define_macro("__VERSION__", "\"ncc 1.0 compatible\"");

    // §11.10 GCC atomics — order constants and __sync_* advertisement
    // (Q15: __STDC_NO_ATOMICS__ + __sync_* coexist deliberately)
    define_macro("__ATOMIC_RELAXED", "0");
    define_macro("__ATOMIC_CONSUME", "1");
    define_macro("__ATOMIC_ACQUIRE", "2");
    define_macro("__ATOMIC_RELEASE", "3");
    define_macro("__ATOMIC_ACQ_REL", "4");
    define_macro("__ATOMIC_SEQ_CST", "5");
    define_macro("__atomic_load_n(p,o)", "(*(p))");
    define_macro("__atomic_store_n(p,v,o)", "(*(p)=(v))");
    define_macro("__atomic_exchange_n(p,v,o)", "__sync_lock_test_and_set(p,v)");
    define_macro("__atomic_compare_exchange_n(p,e,d,w,s,f)",
                 "__sync_bool_compare_and_swap(p,*(e),d)");
    define_macro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1", "1");
    define_macro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_2", "1");
    define_macro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4", "1");
    define_macro("__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8", "1");

    // §11.11 GCC keyword aliases
    define_macro("__alignof__", "_Alignof");
    define_macro("__const__", "const");
    define_macro("__const", "const");
    define_macro("__inline__", "inline");
    define_macro("__inline", "inline");
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

    // §11.12 Type aliases (simplified)
    define_macro("__uint128_t", "unsigned long");
    define_macro("__int128_t", "long");
    define_macro("__int128", "long");
    define_macro("_Float16", "float");
    define_macro("__builtin_va_list", "void *");

    // §11.13 Builtin function-like macros
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
    define_macro("__builtin_offsetof(type,member)",
                 "((unsigned long)&((type*)0)->member)");
    define_macro("__builtin_unreachable()", "((void)0)");
    define_macro("__builtin_assume(x)", "((void)0)");
    define_macro("__builtin_trap()", "abort()");
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
    define_macro("__builtin_printf", "printf");
    define_macro("__builtin_sprintf", "sprintf");
    define_macro("__builtin_putchar(c)", "putchar(c)");
    define_macro("__builtin_puts(s)", "puts(s)");
    define_macro("__builtin_signbit(x)", "((x) < 0)");
    define_macro("__builtin_signbitf(x)", "((x) < 0)");
    define_macro("__builtin_signbitl(x)", "((x) < 0)");

    // §11.14 _Pragma operator
    define_macro("_Pragma(x)", "");

    // §11.15 __DATE__ / __TIME__ — fixed at init time per §13
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

    // §11.16 Handler macros
    Macro *m;
    m = add_macro("__FILE__", true, NULL);       m->handler = file_macro;
    m = add_macro("__LINE__", true, NULL);       m->handler = line_macro;
    m = add_macro("__COUNTER__", true, NULL);    m->handler = counter_macro;
    m = add_macro("__TIMESTAMP__", true, NULL);  m->handler = timestamp_macro;
    m = add_macro("__BASE_FILE__", true, NULL);  m->handler = base_file_macro;
}

// Suppress -Wunused-function for helpers awaiting their callers in
// Chunks 4-7.  This array shrinks as later chunks land.
static void *_v2_pending_uses[] __attribute__((unused)) = {
    (void *)is_hash,
    (void *)skip_line,
    (void *)copy_token,
    (void *)new_eof,
    (void *)append,
    (void *)copy_line,
    (void *)copy_token_list,
    (void *)hideset_intersection,
    (void *)hideset_contains,
    (void *)add_hideset,
    (void *)find_macro,
    (void *)&cond_incl,
    (void *)&pragma_handler,
};
