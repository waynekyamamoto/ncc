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
// Stubs for the remaining public API.  Filled in by later chunks.
//

void define_macro(char *name, char *buf) {
    (void)name; (void)buf;
    // Chunk 3 — calls tokenize on the body and add_macro to register.
}

void init_macros(void) {
    // Chunk 3 fills in spec §11.
}

// Suppress -Wunused-function for helpers and handlers awaiting their
// callers in later chunks.  Each entry is referenced in Chunks 4-7.
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
    (void *)add_macro,
    (void *)file_macro,
    (void *)line_macro,
    (void *)counter_macro,
    (void *)timestamp_macro,
    (void *)base_file_macro,
    (void *)&cond_incl,
    (void *)&pragma_handler,
};
