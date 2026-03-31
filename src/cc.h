// cc.h — Main header for the C compiler
// Targets ARM64 / macOS (Apple Silicon)

#ifndef CC_H
#define CC_H

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

//
// Forward declarations
//

typedef struct Type Type;
typedef struct Node Node;
typedef struct Member Member;
typedef struct Relocation Relocation;
typedef struct Hideset Hideset;
typedef struct Token Token;
typedef struct Obj Obj;
typedef struct File File;
typedef struct NLGoto NLGoto;

//
// alloc.c
//

// Arena allocator for bulk allocation
typedef struct Arena Arena;
struct Arena {
  char *buf;
  int size;
  int capacity;
  Arena *next;
};

void *arena_alloc(Arena *arena, int size);

// General-purpose allocator wrappers
void *calloc_checked(int count, int size);
char *strndup_checked(const char *s, int n);
char *format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

//
// Strings / utility
//

typedef struct {
  char **data;
  int len;
  int capacity;
} StringArray;

void strarray_push(StringArray *arr, char *s);

//
// tokenize.c
//

// Token kinds
typedef enum {
  TK_IDENT,    // Identifiers
  TK_PUNCT,    // Punctuation / operators
  TK_KEYWORD,  // Keywords
  TK_STR,      // String literals
  TK_NUM,      // Numeric literals
  TK_PP_NUM,   // Preprocessing numbers
  TK_EOF,      // End of file
} TokenKind;

// Source file
struct File {
  char *name;       // filename
  int file_no;      // file number (for .file directive)
  char *contents;   // file contents
  // For #line directive
  char *display_name;
  int line_delta;
};

// Token
struct Token {
  TokenKind kind;
  Token *next;
  int64_t val;        // Integer value (for TK_NUM)
  long double fval;   // Float value (for TK_NUM)
  char *loc;          // Token start position in source
  int len;            // Token length
  Type *ty;           // Type (for TK_NUM and TK_STR)
  char *str;          // String literal contents (for TK_STR)

  File *file;         // Source file
  int line_no;        // Line number
  int line_delta;     // Line delta from #line
  bool at_bol;        // True if at beginning of line
  bool has_space;     // True if preceded by whitespace
  Hideset *hideset;   // For macro expansion
  Token *origin;      // Original token before macro expansion
};

// Error reporting
_Noreturn void error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void error_at(const char *loc, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
_Noreturn void error_tok(Token *tok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void warn_tok(Token *tok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Token operations
bool equal(Token *tok, const char *op);
Token *skip(Token *tok, const char *op);
bool consume(Token **rest, Token *tok, const char *str);
void convert_pp_tokens(Token *tok);

// Tokenization
File *new_file(char *name, int file_no, char *contents);
Token *tokenize(File *file);
Token *tokenize_file(char *path);
Token *tokenize_string_literal(Token *tok, Type *basety);

// File utilities
File **get_input_files(void);
File *get_file(Token *tok);

#define unreachable() \
  error("internal error at %s:%d", __FILE__, __LINE__)

//
// preprocess.c
//

Token *preprocess(Token *tok);

// Macro definition callback for -D option
void define_macro(char *name, char *buf);
void undef_macro(char *name);

// Include path management
void add_include_path(char *path);
void init_macros(void);

// Pragma handler
typedef void PragmaHandler(Token *tok);
void set_pragma_handler(PragmaHandler *fn);

//
// parse.c
//

// AST node kinds
typedef enum {
  ND_NULL_EXPR,   // Do nothing
  ND_ADD,         // +
  ND_SUB,         // -
  ND_MUL,         // *
  ND_DIV,         // /
  ND_MOD,         // %
  ND_NEG,         // unary -
  ND_BITAND,      // &
  ND_BITOR,       // |
  ND_BITXOR,      // ^
  ND_BITNOT,      // ~
  ND_SHL,         // <<
  ND_SHR,         // >>
  ND_EQ,          // ==
  ND_NE,          // !=
  ND_LT,          // <
  ND_LE,          // <=
  ND_LOGAND,      // &&
  ND_LOGOR,       // ||
  ND_NOT,         // !
  ND_ASSIGN,      // =
  ND_COND,        // ?:
  ND_COMMA,       // ,
  ND_MEMBER,      // . (struct member access)
  ND_ADDR,        // & (address of)
  ND_DEREF,       // * (dereference)
  ND_RETURN,      // return
  ND_IF,          // if
  ND_FOR,         // for or while
  ND_DO,          // do-while
  ND_SWITCH,      // switch
  ND_CASE,        // case
  ND_BLOCK,       // { ... }
  ND_GOTO,        // goto
  ND_GOTO_EXPR,   // goto *expr (GCC extension)
  ND_LABEL,       // labeled statement
  ND_LABEL_VAL,   // &&label (GCC extension)
  ND_FUNCALL,     // function call
  ND_EXPR_STMT,   // expression statement
  ND_STMT_EXPR,   // statement expression ({ ... })
  ND_VAR,         // variable
  ND_VLA_PTR,     // VLA designator
  ND_NUM,         // integer constant
  ND_CAST,        // type cast
  ND_MEMZERO,     // zero-fill memory
  ND_ASM,         // asm (inline assembly)
  ND_CAS,         // atomic compare-and-swap
  ND_EXCH,        // atomic exchange
  ND_BUILTIN_CLZ,      // __builtin_clz
  ND_BUILTIN_CTZ,      // __builtin_ctz
  ND_BUILTIN_FFS,      // __builtin_ffs
  ND_BUILTIN_POPCOUNT, // __builtin_popcount
  ND_BUILTIN_BSWAP32,  // __builtin_bswap32
  ND_BUILTIN_BSWAP64,  // __builtin_bswap64
  ND_BUILTIN_CLRSB,    // __builtin_clrsb
  ND_BUILTIN_PARITY,   // __builtin_parity
  ND_BUILTIN_ALLOCA,   // __builtin_alloca / alloca
  ND_REAL,             // __real__ (extract real part of complex)
  ND_IMAG,             // __imag__ (extract imaginary part of complex)
  ND_CHAIN_VAR,        // access outer variable via static chain pointer
  ND_FRAME_ADDR,       // current function's frame pointer (x29)
  ND_RETURN_ADDR,      // __builtin_return_address(level)
  ND_BUILTIN_FRAME_ADDR, // __builtin_frame_address(level)
  ND_BUILTIN_ADD_OVERFLOW, // __builtin_add_overflow(a, b, result_ptr)
  ND_BUILTIN_SUB_OVERFLOW, // __builtin_sub_overflow(a, b, result_ptr)
  ND_BUILTIN_MUL_OVERFLOW, // __builtin_mul_overflow(a, b, result_ptr)
  ND_BUILTIN_SETJMP,    // __builtin_setjmp(buf)
  ND_BUILTIN_LONGJMP,   // __builtin_longjmp(buf, val)
  ND_TRAMPOLINE,       // create trampoline for nested function
} NodeKind;

// AST node
struct Node {
  NodeKind kind;
  Node *next;       // Next node (for statement lists)
  Type *ty;         // Type of this node
  Token *tok;       // Representative token

  Node *lhs;        // Left child
  Node *rhs;        // Right child

  // "if", "for", "while", "do"
  Node *cond;
  Node *then;
  Node *els;
  Node *init;
  Node *inc;

  // Block or statement expression
  Node *body;

  // Struct member access
  Member *member;

  // Function call
  char *funcname;
  Type *func_ty;
  Node *args;

  // Goto / label
  char *label;
  char *unique_label;
  char *cont_label;   // for/while continue label
  Node *goto_next;

  // Switch
  Node *case_next;
  Node *default_case;

  // Case
  long begin;       // Case range begin
  long end;         // Case range end

  // Variable
  Obj *var;

  // Numeric literal
  int64_t val;
  long double fval;

  // Overflow builtins
  Type *overflow_ty;

  // Inline assembly
  char *asm_str;
  // Asm operands: arrays of constraint/expression pairs
  int asm_num_outputs;
  int asm_num_inputs;
  char **asm_output_constraints;  // array of constraint strings
  Node **asm_output_exprs;        // array of output expression nodes
  char **asm_input_constraints;   // array of constraint strings
  Node **asm_input_exprs;         // array of input expression nodes
  int asm_num_clobbers;
  char **asm_clobbers;            // array of clobber strings

  // Atomic compare-and-swap
  Node *cas_addr;
  Node *cas_old;
  Node *cas_new;

  // Non-local goto (nested function → outer label via longjmp)
  Obj *nlgoto_buf;  // jmp_buf variable for non-local goto

  // Multi-level chain access (doubly-nested functions)
  int chain_depth;          // number of intermediate chain hops
  Obj *chain_path[4];      // intermediate chain_param Objs to dereference
};

// Variable or function
struct Obj {
  Obj *next;
  char *name;       // Variable name
  Type *ty;         // Type
  Token *tok;       // Representative token
  bool is_local;    // true = local, false = global
  int align;        // Alignment

  // Local variable
  int offset;       // Offset from frame pointer

  // Global variable / function
  bool is_function;
  bool is_definition;
  bool is_static;
  bool is_tentative;
  bool is_tls;        // thread-local storage
  bool is_inline;
  char *init_data;     // Initial data for globals
  int init_data_size;  // Size of init_data (may exceed ty->size for flexible array members)
  Relocation *rel;     // Relocations for initial data
  char *section;       // Section name
  char *visibility;    // Visibility
  char *alias_target;  // alias attribute target name

  // Function body
  Obj *params;
  Node *body;
  Obj *locals;
  Obj *va_area;        // __va_area__ for variadic functions
  Obj *alloca_bottom;  // __alloca_bottom__
  int stack_size;
  bool is_variadic;

  // Nested function support
  bool is_nested;        // true if this is a nested function
  Obj *chain_param;      // hidden parameter holding outer's frame pointer
  Obj *enclosing_fn;     // the enclosing function (for nested functions)
  NLGoto *nlgoto_targets; // non-local goto targets to set up in prologue

  // Static inline function
  bool is_live;
  bool is_root;
  StringArray refs;     // Functions referenced
};

// Non-local goto target: set up in function prologue
struct NLGoto {
  NLGoto *next;
  Obj *buf;           // jmp_buf variable (24 bytes: [fp, sp, label_addr])
  char *unique_label; // target label's assembly name
};

// Relocation entry for global variable initializers
struct Relocation {
  Relocation *next;
  int offset;
  char **label;
  long addend;
};

// Struct/union member
struct Member {
  Member *next;
  Type *ty;
  Token *tok;
  Token *name;
  int idx;
  int align;
  long offset;

  // Bitfield
  bool is_bitfield;
  int bit_offset;
  int bit_width;
};

// Scope for parsing
typedef struct Scope Scope;

// Functions
Obj *parse(Token *tok);
Node *new_cast(Node *expr, Type *ty);

//
// type.c
//

typedef enum {
  TY_VOID,
  TY_BOOL,
  TY_CHAR,
  TY_SHORT,
  TY_INT,
  TY_LONG,
  TY_LONGLONG,
  TY_FLOAT,
  TY_DOUBLE,
  TY_LDOUBLE,
  TY_COMPLEX,   // _Complex
  TY_ENUM,
  TY_PTR,
  TY_FUNC,
  TY_ARRAY,
  TY_VLA,      // variable-length array
  TY_VECTOR,   // GCC vector extension (__attribute__((vector_size(N))))
  TY_STRUCT,
  TY_UNION,
} TypeKind;

struct Type {
  TypeKind kind;
  long size;        // sizeof() value
  int align;        // alignment
  bool is_unsigned; // unsigned or signed
  bool is_atomic;   // _Atomic
  bool is_const;    // const qualifier
  bool is_volatile; // volatile qualifier
  Type *origin;     // for type compatibility check

  // Pointer or array
  Type *base;

  // Declaration
  Token *name;
  Token *name_pos;

  // Array
  long array_len;
  // VLA
  Node *vla_len;   // number of elements
  Obj *vla_size;   // sizeof() value (runtime computed)
  Token *vla_dim_tok; // token range for VLA dimension (for re-parsing side effects)

  // Struct / Union
  Member *members;
  bool is_flexible; // flexible array member
  bool is_packed;   // __attribute__((packed))

  // Function type
  Type *return_ty;
  Type *params;
  bool is_variadic;
  Type *next;       // for parameter list
};

// Pre-defined types
extern Type *ty_void;
extern Type *ty_bool;

extern Type *ty_char;
extern Type *ty_short;
extern Type *ty_int;
extern Type *ty_long;
extern Type *ty_longlong;

extern Type *ty_uchar;
extern Type *ty_ushort;
extern Type *ty_uint;
extern Type *ty_ulong;
extern Type *ty_ulonglong;

extern Type *ty_float;
extern Type *ty_double;
extern Type *ty_ldouble;

bool is_integer(Type *ty);
bool is_flonum(Type *ty);
bool is_numeric(Type *ty);
bool is_complex(Type *ty);
bool is_compatible(Type *t1, Type *t2);
Type *copy_type(Type *ty);
Type *pointer_to(Type *base);
Type *func_type(Type *return_ty);
Type *array_of(Type *base, long len);
Type *vla_of(Type *base, Node *len);
Type *enum_type(void);
Type *struct_type(void);
Type *complex_type(Type *base);
Type *vector_of(Type *base, int total_size);
bool is_vector(Type *ty);
void add_type(Node *node);

//
// codegen_arm64.c
//

void codegen(Obj *prog, FILE *out);
long align_to(long n, int align);

//
// unicode.c
//

int encode_utf8(char *buf, uint32_t c);
uint32_t decode_utf8(char **new_pos, char *p);
bool is_ident1(uint32_t c);  // first char of identifier
bool is_ident2(uint32_t c);  // subsequent chars

//
// hashmap.c
//

typedef struct {
  char *key;
  int keylen;
  void *val;
} HashEntry;

typedef struct {
  HashEntry *buckets;
  int capacity;
  int used;
} HashMap;

void *hashmap_get(HashMap *map, const char *key);
void *hashmap_get2(HashMap *map, const char *key, int keylen);
void hashmap_put(HashMap *map, const char *key, void *val);
void hashmap_put2(HashMap *map, const char *key, int keylen, void *val);
void hashmap_delete(HashMap *map, const char *key);
void hashmap_delete2(HashMap *map, const char *key, int keylen);
void hashmap_test(void);

//
// Global state
//

// Base types for the target (ARM64 macOS)
// On ARM64 macOS:
//   char = 1, short = 2, int = 4, long = 8, long long = 8
//   pointer = 8
//   float = 4, double = 8, long double = 8
//   _Bool = 1

// Compilation options
extern StringArray include_paths;
extern bool opt_fpic;
extern bool opt_fcommon;
extern char *base_file;

// Counter for unique labels
extern int label_cnt;

#endif // CC_H
