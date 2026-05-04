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
// Stubs for the remaining public API.  Filled in by later chunks.
//

void define_macro(char *name, char *buf) {
    (void)name; (void)buf;
}

void undef_macro(char *name) {
    (void)name;
}

void init_macros(void) {
    // Chunk 3 fills in spec §11.
}

// Suppress -Wunused-function warnings for helpers awaiting later
// chunks.  Each name listed here is referenced from a future chunk
// (Chunk 2-7); referencing them in this no-op array keeps clang
// happy without changing observable behavior.
static void *_v2_pending_uses[] __attribute__((unused)) = {
    (void *)is_hash,
    (void *)skip_line,
    (void *)copy_token,
    (void *)new_eof,
    (void *)append,
    (void *)copy_line,
    (void *)copy_token_list,
    (void *)&macros,
    (void *)&cond_incl,
    (void *)&pragma_handler,
    (void *)&counter_val,
};
