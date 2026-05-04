// type_v2.c — Phase 3 spec-derived type system (skeleton).
//
// Implementation will be derived from docs/specs/03_type.md under
// strict no-peek discipline (no consultation of src/type.c during
// impl).  Built into the alternate `ncc-v2` binary; validated
// against the canonical `ncc` via bootstrap fixed-point + torture +
// real programs.
//
// This skeleton provides the public symbols declared in cc.h so the
// dual-build links cleanly.  Real implementations land in chunks.

#include "cc.h"

// §4 predefined Type singletons — file-scope compound literals at
// fixed addresses (Q1: identity-compared by callers; do not move
// these into functions).
Type *ty_void      = &(Type){TY_VOID, 1, 1};
Type *ty_bool      = &(Type){TY_BOOL, 1, 1, .is_unsigned = true};
Type *ty_char      = &(Type){TY_CHAR, 1, 1};
Type *ty_short     = &(Type){TY_SHORT, 2, 2};
Type *ty_int       = &(Type){TY_INT, 4, 4};
Type *ty_long      = &(Type){TY_LONG, 8, 8};
Type *ty_longlong  = &(Type){TY_LONGLONG, 8, 8};
Type *ty_uchar     = &(Type){TY_CHAR, 1, 1, .is_unsigned = true};
Type *ty_ushort    = &(Type){TY_SHORT, 2, 2, .is_unsigned = true};
Type *ty_uint      = &(Type){TY_INT, 4, 4, .is_unsigned = true};
Type *ty_ulong     = &(Type){TY_LONG, 8, 8, .is_unsigned = true};
Type *ty_ulonglong = &(Type){TY_LONGLONG, 8, 8, .is_unsigned = true};
Type *ty_float     = &(Type){TY_FLOAT, 4, 4};
Type *ty_double    = &(Type){TY_DOUBLE, 8, 8};
Type *ty_ldouble   = &(Type){TY_LDOUBLE, 8, 8};

// Stubs — Chunks 1-4 fill these in.

bool is_integer(Type *ty) { (void)ty; return false; }
bool is_flonum(Type *ty) { (void)ty; return false; }
bool is_numeric(Type *ty) { (void)ty; return false; }
bool is_complex(Type *ty) { (void)ty; return false; }
bool is_vector(Type *ty) { (void)ty; return false; }
bool is_compatible(Type *t1, Type *t2) { return t1 == t2; }

Type *copy_type(Type *ty) { return ty; }
Type *pointer_to(Type *base) { (void)base; return ty_void; }
Type *func_type(Type *return_ty) { (void)return_ty; return ty_void; }
Type *array_of(Type *base, long len) { (void)base; (void)len; return ty_void; }
Type *vla_of(Type *base, Node *len) { (void)base; (void)len; return ty_void; }
Type *enum_type(void) { return ty_int; }
Type *struct_type(void) { return ty_void; }
Type *vector_of(Type *base, int total_size) { (void)base; (void)total_size; return ty_void; }
Type *complex_type(Type *base) { (void)base; return ty_void; }

void add_type(Node *node) { (void)node; }
