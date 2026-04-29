// Regression: Incomplete-struct array initializer caused calloc(-1, ...) OOM.
//
// Path: parse.c new_initializer treated array_len as size, but for an
// incomplete-struct base type size==0 (not <0). The clamp checked size,
// so array_len==-1 leaked through and calloc allocated SIZE_MAX bytes.
// gvar_initializer also needed a clamp before calloc_checked.
//
// Found in Linux kernel kgdb.c (68-element dbg_reg_def[]).
// Fixed: 2026-04-27 (commit ad2db17).

struct dbg_reg_def {
  char *name;
  int size;
  int offset;
};

// Forward-declared (incomplete) struct used as an array base before completion.
// The empty initializer list with no size lets the compiler size the array.
struct dbg_reg_def dbg_reg_def[] = {
  {"x0", 8, 0},  {"x1", 8, 8},  {"x2", 8, 16}, {"x3", 8, 24},
  {"x4", 8, 32}, {"x5", 8, 40}, {"x6", 8, 48}, {"x7", 8, 56},
};

int main(void) { return 0; }
