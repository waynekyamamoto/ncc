// preprocess_v2.c — Phase 2 spec-derived preprocessor (skeleton)
//
// This is the entry point for Phase 2 of the chibicc swap-out.  The
// implementation is being written from docs/specs/02_preprocessor.md
// under strict no-peek discipline (no consultation of src/preprocess.c
// during impl).  This skeleton provides the public symbols declared in
// cc.h so the dual-build (Makefile) links cleanly; the actual behavior
// is filled in iteratively, with scripts/validate_preprocessor.sh
// ncc-v2 ncc as the contract.
//
// Until §4-§12 of the spec are implemented, ncc-v2 will produce
// degenerate -E output (all directives passed through, no macros
// expanded), and validate_preprocessor.sh will FAIL.  That's the
// expected starting state.

#include "cc.h"

long pp_token_count;

Token *preprocess(Token *tok) {
    // TODO: docs/specs/02_preprocessor.md §4 — top-level entry.
    // Pass through unchanged for now so the pipeline runs end-to-end
    // without crashing; convert_pp_tokens still needs to run so that
    // numeric literals reach the parser as TK_NUM.
    convert_pp_tokens(tok);
    return tok;
}

void define_macro(char *name, char *buf) {
    (void)name;
    (void)buf;
    // TODO: §6.1.
}

void undef_macro(char *name) {
    (void)name;
    // TODO: §6.1.
}

void add_include_path(char *path) {
    (void)path;
    // TODO: §3.1.
}

void init_macros(void) {
    // TODO: §11.  No predefined macros until the table is implemented;
    // callers expecting __STDC__, __APPLE__, etc. will see them as
    // undefined.
}

void set_pragma_handler(PragmaHandler *fn) {
    (void)fn;
    // TODO: §3 / §12.2 (Q22 — invoke the registered callback).
}
