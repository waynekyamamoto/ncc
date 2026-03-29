// type.c — C type system
#include "cc.h"

// ARM64 macOS type sizes:
// char=1, short=2, int=4, long=8, long long=8, pointer=8
// float=4, double=8, long double=8 (not 16 on Apple ARM64)

Type *ty_void = &(Type){TY_VOID, 1, 1};
Type *ty_bool = &(Type){TY_BOOL, 1, 1, .is_unsigned = true};

Type *ty_char = &(Type){TY_CHAR, 1, 1};
Type *ty_short = &(Type){TY_SHORT, 2, 2};
Type *ty_int = &(Type){TY_INT, 4, 4};
Type *ty_long = &(Type){TY_LONG, 8, 8};
Type *ty_longlong = &(Type){TY_LONGLONG, 8, 8};

Type *ty_uchar = &(Type){TY_CHAR, 1, 1, .is_unsigned = true};
Type *ty_ushort = &(Type){TY_SHORT, 2, 2, .is_unsigned = true};
Type *ty_uint = &(Type){TY_INT, 4, 4, .is_unsigned = true};
Type *ty_ulong = &(Type){TY_LONG, 8, 8, .is_unsigned = true};
Type *ty_ulonglong = &(Type){TY_LONGLONG, 8, 8, .is_unsigned = true};

Type *ty_float = &(Type){TY_FLOAT, 4, 4};
Type *ty_double = &(Type){TY_DOUBLE, 8, 8};
Type *ty_ldouble = &(Type){TY_LDOUBLE, 8, 8};  // 8 bytes on ARM64 macOS

bool is_integer(Type *ty) {
  TypeKind k = ty->kind;
  return k == TY_BOOL || k == TY_CHAR || k == TY_SHORT ||
         k == TY_INT || k == TY_LONG || k == TY_LONGLONG ||
         k == TY_ENUM;
}

bool is_flonum(Type *ty) {
  return ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE ||
         ty->kind == TY_LDOUBLE;
}

bool is_numeric(Type *ty) {
  return is_integer(ty) || is_flonum(ty);
}

bool is_complex(Type *ty) {
  return ty->kind == TY_COMPLEX;
}

bool is_compatible(Type *t1, Type *t2) {
  if (t1 == t2)
    return true;

  if (t1->origin)
    return is_compatible(t1->origin, t2);
  if (t2->origin)
    return is_compatible(t1, t2->origin);

  if (t1->kind != t2->kind)
    return false;

  switch (t1->kind) {
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_LONG:
  case TY_LONGLONG:
    return t1->is_unsigned == t2->is_unsigned;
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
    return true;
  case TY_PTR:
    return is_compatible(t1->base, t2->base);
  case TY_FUNC: {
    if (!is_compatible(t1->return_ty, t2->return_ty))
      return false;
    if (t1->is_variadic != t2->is_variadic)
      return false;
    Type *p1 = t1->params;
    Type *p2 = t2->params;
    for (; p1 && p2; p1 = p1->next, p2 = p2->next)
      if (!is_compatible(p1, p2))
        return false;
    return p1 == NULL && p2 == NULL;
  }
  case TY_ARRAY:
    if (!is_compatible(t1->base, t2->base))
      return false;
    return t1->array_len < 0 || t2->array_len < 0 ||
           t1->array_len == t2->array_len;
  default:
    return false;
  }
}

Type *copy_type(Type *ty) {
  Type *ret = calloc_checked(1, sizeof(Type));
  *ret = *ty;
  ret->origin = ty;
  ret->next = NULL;
  return ret;
}

Type *pointer_to(Type *base) {
  Type *ty = calloc_checked(1, sizeof(Type));
  ty->kind = TY_PTR;
  ty->size = 8;   // 64-bit pointers
  ty->align = 8;
  ty->base = base;
  return ty;
}

Type *func_type(Type *return_ty) {
  // Functions are "incomplete" — they are not sized.
  // They are always used via pointer.
  Type *ty = calloc_checked(1, sizeof(Type));
  ty->kind = TY_FUNC;
  ty->return_ty = return_ty;
  return ty;
}

Type *array_of(Type *base, int len) {
  Type *ty = calloc_checked(1, sizeof(Type));
  ty->kind = TY_ARRAY;
  ty->size = base->size * len;
  ty->align = base->align;
  ty->base = base;
  ty->array_len = len;
  return ty;
}

Type *vla_of(Type *base, Node *len) {
  Type *ty = calloc_checked(1, sizeof(Type));
  ty->kind = TY_VLA;
  ty->size = 8;   // VLA variables are stored as pointers
  ty->align = 8;
  ty->base = base;
  ty->vla_len = len;
  return ty;
}

Type *enum_type(void) {
  Type *ty = calloc_checked(1, sizeof(Type));
  ty->kind = TY_ENUM;
  ty->size = 4;
  ty->align = 4;
  return ty;
}

Type *struct_type(void) {
  Type *ty = calloc_checked(1, sizeof(Type));
  ty->kind = TY_STRUCT;
  return ty;
}

// Create a _Complex type from a base scalar type.
// _Complex double = 16 bytes (two doubles), etc.
Type *complex_type(Type *base) {
  Type *ty = calloc_checked(1, sizeof(Type));
  ty->kind = TY_COMPLEX;
  ty->base = base;
  ty->size = base->size * 2;
  ty->align = base->align;
  return ty;
}

// Get the common type for binary operations (usual arithmetic conversions).
static Type *get_common_type(Type *ty1, Type *ty2) {
  if (ty1->base && ty1->kind != TY_COMPLEX)
    return pointer_to(ty1->base);

  // Complex type promotions: if either is complex, result is complex
  if (ty1->kind == TY_COMPLEX || ty2->kind == TY_COMPLEX) {
    Type *base1 = ty1->kind == TY_COMPLEX ? ty1->base : ty1;
    Type *base2 = ty2->kind == TY_COMPLEX ? ty2->base : ty2;
    Type *common_base = get_common_type(base1, base2);
    return complex_type(common_base);
  }

  // Float promotions
  if (ty1->kind == TY_FUNC)
    return pointer_to(ty1);
  if (ty2->kind == TY_FUNC)
    return pointer_to(ty2);

  if (ty1->kind == TY_LDOUBLE || ty2->kind == TY_LDOUBLE)
    return ty_ldouble;
  if (ty1->kind == TY_DOUBLE || ty2->kind == TY_DOUBLE)
    return ty_double;
  if (ty1->kind == TY_FLOAT || ty2->kind == TY_FLOAT)
    return ty_float;

  // Integer promotion: anything smaller than int becomes int
  if (ty1->size < 4)
    ty1 = ty_int;
  if (ty2->size < 4)
    ty2 = ty_int;

  // If one type is larger, use it
  if (ty1->size != ty2->size)
    return (ty1->size < ty2->size) ? ty2 : ty1;

  // Same size: unsigned wins
  if (ty2->is_unsigned)
    return ty2;
  return ty1;
}

// Perform usual arithmetic conversions on operands of a binary expression.
// Insert implicit casts as needed.
static void usual_arith_conv(Node **lhs, Node **rhs) {
  Type *ty = get_common_type((*lhs)->ty, (*rhs)->ty);
  *lhs = new_cast(*lhs, ty);
  *rhs = new_cast(*rhs, ty);
}

// Add type information to all nodes in the AST.
void add_type(Node *node) {
  if (!node || node->ty)
    return;

  add_type(node->lhs);
  add_type(node->rhs);
  add_type(node->cond);
  add_type(node->then);
  add_type(node->els);
  add_type(node->init);
  add_type(node->inc);

  for (Node *n = node->body; n; n = n->next)
    add_type(n);
  for (Node *n = node->args; n; n = n->next)
    add_type(n);

  switch (node->kind) {
  case ND_NUM:
    if (!node->ty)
      node->ty = ty_int;
    return;
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
    usual_arith_conv(&node->lhs, &node->rhs);
    node->ty = node->lhs->ty;
    return;
  case ND_NEG: {
    Type *ty = node->lhs->ty;
    if (is_integer(ty) && ty->size < 4)
      ty = ty_int;
    node->ty = ty;
    return;
  }
  case ND_ASSIGN:
    if (node->lhs->ty->kind == TY_ARRAY)
      error_tok(node->lhs->tok, "not an lvalue");
    if (node->lhs->ty->kind != TY_STRUCT && node->lhs->ty->kind != TY_COMPLEX)
      node->rhs = new_cast(node->rhs, node->lhs->ty);
    node->ty = node->lhs->ty;
    return;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
    usual_arith_conv(&node->lhs, &node->rhs);
    node->ty = ty_int;
    return;
  case ND_FUNCALL:
    node->ty = node->func_ty->return_ty;
    return;
  case ND_NOT:
  case ND_LOGAND:
  case ND_LOGOR:
    node->ty = ty_int;
    return;
  case ND_BITNOT:
  case ND_SHL:
  case ND_SHR: {
    Type *ty = node->lhs->ty;
    if (is_integer(ty) && ty->size < 4)
      ty = ty_int;
    node->ty = ty;
    return;
  }
  case ND_VAR:
  case ND_VLA_PTR:
    node->ty = node->var->ty;
    if (node->ty->kind == TY_VLA)
      node->ty = pointer_to(node->ty->base);
    return;
  case ND_COND:
    if (node->then->ty->kind == TY_VOID || node->els->ty->kind == TY_VOID) {
      node->ty = ty_void;
    } else {
      usual_arith_conv(&node->then, &node->els);
      node->ty = node->then->ty;
    }
    return;
  case ND_COMMA:
    node->ty = node->rhs->ty;
    return;
  case ND_MEMBER:
    node->ty = node->member->ty;
    if (node->member->is_bitfield) {
      int bw = node->member->bit_width;
      if (node->ty->is_unsigned) {
        if (bw < 32)
          node->ty = ty_int;
        else if (bw == 32)
          node->ty = ty_uint;
      } else {
        if (bw <= 32)
          node->ty = ty_int;
      }
    }
    return;
  case ND_ADDR: {
    Type *ty = node->lhs->ty;
    if (ty->kind == TY_ARRAY)
      node->ty = pointer_to(ty->base);
    else
      node->ty = pointer_to(ty);
    return;
  }
  case ND_DEREF:
    if (!node->lhs->ty->base)
      error_tok(node->tok, "invalid pointer dereference");
    if (node->lhs->ty->base->kind == TY_VOID)
      error_tok(node->tok, "dereferencing a void pointer");

    node->ty = node->lhs->ty->base;
    // Note: VLA types stay as VLA here (like arrays, they're address types).
    // load() will skip the load for VLA types, and VLA-to-pointer decay
    // happens in new_add/new_sub when used in pointer arithmetic.
    return;
  case ND_STMT_EXPR:
    if (node->body) {
      Node *stmt = node->body;
      while (stmt->next)
        stmt = stmt->next;
      if (stmt->kind == ND_EXPR_STMT) {
        node->ty = stmt->lhs->ty;
        return;
      }
    }
    error_tok(node->tok, "statement expression returning void is not supported");
    return;
  case ND_LABEL_VAL:
    node->ty = pointer_to(ty_void);
    return;
  case ND_REAL:
  case ND_IMAG:
    if (node->lhs->ty->kind == TY_COMPLEX)
      node->ty = node->lhs->ty->base;
    else
      node->ty = node->lhs->ty;  // for non-complex, __real__/__imag__ is identity/zero
    return;
  case ND_CAS:
    add_type(node->cas_addr);
    add_type(node->cas_old);
    add_type(node->cas_new);
    node->ty = ty_bool;
    return;
  case ND_EXCH:
    add_type(node->cas_addr);
    add_type(node->cas_new);
    node->ty = node->cas_addr->ty->base;
    return;
  default:
    break;
  }
}
