// codegen_arm64.c — ARM64 (Apple Silicon / macOS) code generator
// Generates ARM64 assembly in GNU as syntax for Mach-O.
#include "cc.h"

// Output file
static FILE *output_file;
static int depth;        // Stack depth tracker
static Obj *current_fn;
static char *argreg8[] = {"w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7"};
static char *argreg64[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
static char *fpreg[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
// static char *fpreg32[] = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7"};

long align_to(long n, int align) {
  return (n + align - 1) / align * align;
}

#define println(...)  do { fprintf(output_file, "\t"); fprintf(output_file, __VA_ARGS__); fprintf(output_file, "\n"); } while(0)
#define printlabel(...) do { fprintf(output_file, __VA_ARGS__); fprintf(output_file, "\n"); } while(0)

static void gen_expr(Node *node);
static void gen_stmt(Node *node);
static void gen_stmt_dead(Node *node);
static void gen_addr(Node *node);
static void push(void);
static void pop(char *reg);
static void pushf(void);
static void popf(int reg);

// Generate a boolean condition test for a node.
// After gen_expr, the value is in x0 (integer) or d0 (float).
// This emits cmp/fcmp and the caller can use b.eq/b.ne.
static void gen_cond(Node *node) {
  gen_expr(node);
  add_type(node);
  if (node->ty->kind == TY_COMPLEX) {
    // Complex truthiness: nonzero if real != 0 || imag != 0
    // x0 holds address of the complex value
    int base_sz = node->ty->base->size;
    println("ldr d1, [x0, #%d]", base_sz); // imag part
    println("ldr d0, [x0]");               // real part
    println("fcmp d0, #0.0");
    println("cset w0, ne");
    println("fcmp d1, #0.0");
    println("cset w1, ne");
    println("orr w0, w0, w1");
    println("cmp x0, #0");
  } else if (is_flonum(node->ty)) {
    println("fcmp d0, #0.0");
    println("cset w0, ne");
    println("cmp x0, #0");
  } else {
    println("cmp x0, #0");
  }
}

//
// Stack operations — we use x0 as accumulator, stack for intermediate values
//

static void push(void) {
  println("str x0, [sp, #-16]!");
  depth++;
}

static void pop(char *reg) {
  println("ldr %s, [sp], #16", reg);
  depth--;
}

// Push float from d0
static void pushf(void) {
  println("str d0, [sp, #-16]!");
  depth++;
}

// Pop float into d<reg>
static void popf(int reg) {
  println("ldr d%d, [sp], #16", reg);
  depth--;
}

// Emit instructions to load an arbitrary 64-bit immediate into a register.
// Uses movz+movk sequence for values that don't fit in a single MOV.
static void load_imm(char *reg, uint64_t val) {
  if (val == 0) {
    println("mov %s, #0", reg);
    return;
  }

  // Check if it fits in a simple MOV (16-bit unsigned)
  if (val <= 0xFFFF) {
    println("mov %s, #%llu", reg, (unsigned long long)val);
    return;
  }

  // Check if it's a small negative (fits in MOV with sign extension)
  if ((int64_t)val >= -0x10000 && (int64_t)val < 0) {
    println("mov %s, #%lld", reg, (long long)(int64_t)val);
    return;
  }

  // Need movz + movk sequence
  println("movz %s, #%llu", reg, (unsigned long long)(val & 0xFFFF));
  if ((val >> 16) & 0xFFFF)
    println("movk %s, #%llu, lsl #16", reg, (unsigned long long)((val >> 16) & 0xFFFF));
  if ((val >> 32) & 0xFFFF)
    println("movk %s, #%llu, lsl #32", reg, (unsigned long long)((val >> 32) & 0xFFFF));
  if ((val >> 48) & 0xFFFF)
    println("movk %s, #%llu, lsl #48", reg, (unsigned long long)((val >> 48) & 0xFFFF));
}

// Emit a double constant into d-register.
__attribute__((unused))
static void emit_double_const(int dreg, double val) {
  union { double d; uint64_t u; } v;
  v.d = val;
  if (v.u == 0) {
    println("movi d%d, #0", dreg);
  } else {
    println("mov x9, #%llu", v.u & 0xFFFF);
    if ((v.u >> 16) & 0xFFFF)
      println("movk x9, #%llu, lsl #16", (v.u >> 16) & 0xFFFF);
    if ((v.u >> 32) & 0xFFFF)
      println("movk x9, #%llu, lsl #32", (v.u >> 32) & 0xFFFF);
    if ((v.u >> 48) & 0xFFFF)
      println("movk x9, #%llu, lsl #48", (v.u >> 48) & 0xFFFF);
    println("fmov d%d, x9", dreg);
  }
}

// Load a scalar from [x0 + offset] into d0, respecting the base type.
static void load_complex_part(Type *base, int offset) {
  if (base->kind == TY_FLOAT) {
    println("ldr s0, [x0, #%d]", offset);
    println("fcvt d0, s0");
  } else if (base->kind == TY_DOUBLE || base->kind == TY_LDOUBLE) {
    println("ldr d0, [x0, #%d]", offset);
  } else {
    // Integer complex
    if (base->size == 1)
      println(base->is_unsigned ? "ldrb w0, [x0, #%d]" : "ldrsb w0, [x0, #%d]", offset);
    else if (base->size == 2)
      println(base->is_unsigned ? "ldrh w0, [x0, #%d]" : "ldrsh w0, [x0, #%d]", offset);
    else if (base->size == 4)
      println("ldr w0, [x0, #%d]", offset);
    else
      println("ldr x0, [x0, #%d]", offset);
    if (base->size == 4 && !base->is_unsigned)
      println("sxtw x0, w0");
  }
}

// Load value from memory according to type
static void load(Type *ty) {
  switch (ty->kind) {
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION:
  case TY_FUNC:
  case TY_VLA:
  case TY_COMPLEX:
  case TY_VECTOR:
    // These are already addresses — no dereference needed
    return;
  case TY_FLOAT:
    println("ldr s0, [x0]");
    println("fcvt d0, s0");
    return;
  case TY_DOUBLE:
  case TY_LDOUBLE:
    println("ldr d0, [x0]");
    return;
  default:
    break;
  }

  char *insn;
  if (ty->size == 1)
    insn = ty->is_unsigned ? "ldrb w0, [x0]" : "ldrsb w0, [x0]";
  else if (ty->size == 2)
    insn = ty->is_unsigned ? "ldrh w0, [x0]" : "ldrsh w0, [x0]";
  else if (ty->size == 4)
    insn = ty->is_unsigned ? "ldr w0, [x0]" : "ldr w0, [x0]"; // sign ext to 64-bit not needed for w reg
  else
    insn = "ldr x0, [x0]";

  println("%s", insn);

  // Sign-extend 32-bit to 64-bit if needed for signed int
  if (ty->size == 4 && !ty->is_unsigned)
    println("sxtw x0, w0");
}

// Store x0/d0 to address on top of stack
static void store(Type *ty) {
  pop("x1"); // address

  switch (ty->kind) {
  case TY_STRUCT:
  case TY_UNION:
  case TY_COMPLEX:
  case TY_VECTOR: {
    // Copy struct/complex/vector from x0 (src addr) to x1 (dst addr)

    // For structs with VLA members, use a runtime-sized copy loop
    if (ty->vla_size && (ty->kind == TY_STRUCT || ty->kind == TY_UNION)) {
      int size_off = -ty->vla_size->offset;
      if (size_off <= 4095)
        println("sub x3, x29, #%d", size_off);
      else {
        load_imm("x3", (uint64_t)size_off);
        println("sub x3, x29, x3");
      }
      println("ldr x3, [x3]");  // x3 = runtime byte count
      // Runtime byte-copy loop: copy x3 bytes from [x0] to [x1]
      int c = label_cnt++;
      println("mov x4, #0");                      // x4 = offset
      printlabel(".L.vla.copy.%d:", c);
      println("cmp x4, x3");
      println("b.ge .L.vla.copy.end.%d", c);
      println("ldrb w5, [x0, x4]");
      println("strb w5, [x1, x4]");
      println("add x4, x4, #1");
      println("b .L.vla.copy.%d", c);
      printlabel(".L.vla.copy.end.%d:", c);
      return;
    }

    // Use 8-byte copies for bulk, then byte copies for the tail.
    // For large structs, periodically advance base pointers to keep
    // offsets within ARM64 immediate range (strb: 0-4095, str x: 0-32760).
    int copied = 0;
    int off = 0;
    while (copied + 8 <= ty->size) {
      if (off + 8 > 4088) {
        // Advance base pointers to keep offsets in range
        println("add x0, x0, #%d", off);
        println("add x1, x1, #%d", off);
        off = 0;
      }
      println("ldr x2, [x0, #%d]", off);
      println("str x2, [x1, #%d]", off);
      off += 8;
      copied += 8;
    }
    while (copied < ty->size) {
      if (off >= 4096) {
        println("add x0, x0, #%d", off);
        println("add x1, x1, #%d", off);
        off = 0;
      }
      println("ldrb w2, [x0, #%d]", off);
      println("strb w2, [x1, #%d]", off);
      off++;
      copied++;
    }
    return;
  }
  case TY_FLOAT:
    println("fcvt s0, d0");
    println("str s0, [x1]");
    return;
  case TY_DOUBLE:
  case TY_LDOUBLE:
    println("str d0, [x1]");
    return;
  default:
    break;
  }

  if (ty->size == 1)
    println("strb w0, [x1]");
  else if (ty->size == 2)
    println("strh w0, [x1]");
  else if (ty->size == 4)
    println("str w0, [x1]");
  else
    println("str x0, [x1]");
}

// Store x0/d0 to address in x1 (no pop)
__attribute__((unused))
static void store_to(Type *ty, char *addr_reg) {
  switch (ty->kind) {
  case TY_STRUCT:
  case TY_UNION:
  case TY_VECTOR: {
    int copied = 0;
    int off = 0;
    while (copied + 8 <= ty->size) {
      if (off + 8 > 4088) {
        println("add x0, x0, #%d", off);
        println("add %s, %s, #%d", addr_reg, addr_reg, off);
        off = 0;
      }
      println("ldr x2, [x0, #%d]", off);
      println("str x2, [%s, #%d]", addr_reg, off);
      off += 8;
      copied += 8;
    }
    while (copied < ty->size) {
      if (off >= 4096) {
        println("add x0, x0, #%d", off);
        println("add %s, %s, #%d", addr_reg, addr_reg, off);
        off = 0;
      }
      println("ldrb w2, [x0, #%d]", off);
      println("strb w2, [%s, #%d]", addr_reg, off);
      off++;
      copied++;
    }
    return;
  }
  case TY_FLOAT:
    println("fcvt s0, d0");
    println("str s0, [%s]", addr_reg);
    return;
  case TY_DOUBLE:
  case TY_LDOUBLE:
    println("str d0, [%s]", addr_reg);
    return;
  default:
    break;
  }

  if (ty->size == 1)
    println("strb w0, [%s]", addr_reg);
  else if (ty->size == 2)
    println("strh w0, [%s]", addr_reg);
  else if (ty->size == 4)
    println("str w0, [%s]", addr_reg);
  else
    println("str x0, [%s]", addr_reg);
}

// Emit code to compute the address of a node
static void gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    // Variable address
    if (node->var->is_local) {
      // Local variable: offset from frame pointer (always negative)
      if (node->var->offset < 0) {
        int off = -node->var->offset;
        if (off <= 4095) {
          println("sub x0, x29, #%d", off);
        } else {
          load_imm("x0", (uint64_t)off);
          println("sub x0, x29, x0");
        }
      } else {
        if (node->var->offset <= 4095) {
          println("add x0, x29, #%d", node->var->offset);
        } else {
          load_imm("x0", (uint64_t)node->var->offset);
          println("add x0, x29, x0");
        }
      }
      // For struct/union with VLA members: the frame slot stores a pointer
      // to the dynamically allocated struct. Load it to get the actual address.
      if ((node->var->ty->kind == TY_STRUCT || node->var->ty->kind == TY_UNION) &&
          node->var->ty->vla_size) {
        println("ldr x0, [x0]");
      }
    } else if (!node->var->is_definition) {
      // External variable: use GOT for PIC on macOS
      println("adrp x0, _%s@GOTPAGE", node->var->name);
      println("ldr x0, [x0, _%s@GOTPAGEOFF]", node->var->name);
    } else {
      // Local global variable: PIC addressing on macOS
      println("adrp x0, _%s@PAGE", node->var->name);
      println("add x0, x0, _%s@PAGEOFF", node->var->name);
    }
    return;

  case ND_DEREF:
    gen_expr(node->lhs);
    return;

  case ND_COMMA:
    gen_expr(node->lhs);
    gen_addr(node->rhs);
    return;

  case ND_MEMBER:
    gen_addr(node->lhs);
    if (node->member->offset <= 4095) {
      println("add x0, x0, #%ld", node->member->offset);
    } else {
      load_imm("x1", (uint64_t)node->member->offset);
      println("add x0, x0, x1");
    }
    return;

  case ND_FUNCALL:
    gen_expr(node);
    return;

  case ND_STMT_EXPR:
    // Statement expressions that produce struct/union: gen_expr leaves address in x0
    gen_expr(node);
    return;

  case ND_REAL:
    // Address of __real__ expr: address of the real part
    gen_addr(node->lhs);
    return;

  case ND_IMAG:
    // Address of __imag__ expr: address of the imaginary part
    gen_addr(node->lhs);
    if (is_complex(node->lhs->ty))
      println("add x0, x0, #%ld", node->lhs->ty->base->size);
    return;

  case ND_ASSIGN:
  case ND_COND:
    // For compound literals that result in assignments
    gen_expr(node);
    return;

  case ND_VLA_PTR:
    if (node->var->is_local) {
      println("sub x0, x29, #%d", -node->var->offset);
    }
    return;

  case ND_CHAIN_VAR: {
    gen_expr(node->lhs);  // Load innermost chain pointer (outer fp)
    // Follow intermediate chain pointers for multi-level nesting
    for (int i = 0; i < node->chain_depth; i++) {
      int cp_off = node->chain_path[i]->offset;
      if (cp_off < 0) {
        int abs_off = -cp_off;
        if (abs_off <= 4095) println("sub x0, x0, #%d", abs_off);
        else { load_imm("x9", (uint64_t)abs_off); println("sub x0, x0, x9"); }
      } else if (cp_off > 0) {
        if (cp_off <= 4095) println("add x0, x0, #%d", cp_off);
        else { load_imm("x9", (uint64_t)cp_off); println("add x0, x0, x9"); }
      }
      println("ldr x0, [x0]");  // dereference to get next-level fp
    }
    // Compute final variable address
    int off = node->var->offset;
    if (off < 0) {
      int abs_off = -off;
      if (abs_off <= 4095) println("sub x0, x0, #%d", abs_off);
      else { load_imm("x9", (uint64_t)abs_off); println("sub x0, x0, x9"); }
    } else if (off > 0) {
      if (off <= 4095) println("add x0, x0, #%d", off);
      else { load_imm("x9", (uint64_t)off); println("add x0, x0, x9"); }
    }
    return;
  }

  default:
    error_tok(node->tok, "not an lvalue");
  }
}

// Cast value in x0/d0 to a target type
static void cast(Type *from, Type *to) {
  if (to->kind == TY_VOID)
    return;

  // Vector casts are handled in the parser via memory reinterpretation.
  // If we still get here, it's a no-op (same-size bitwise reinterpret).
  if (from->kind == TY_VECTOR || to->kind == TY_VECTOR)
    return;

  if (to->kind == TY_BOOL) {
    println("cmp x0, #0");
    println("cset w0, ne");
    println("and x0, x0, #1");
    return;
  }

  // Float conversions
  if (is_flonum(from) && is_flonum(to)) {
    // d0 → d0, no conversion needed (both stored as double in d0)
    return;
  }

  if (is_flonum(from) && is_integer(to)) {
    // d0 → x0. Use w0 target for <=32-bit types so ARM64 saturates correctly.
    if (to->size <= 4) {
      if (to->is_unsigned)
        println("fcvtzu w0, d0");
      else
        println("fcvtzs w0, d0");
      println("sxtw x0, w0");
    } else {
      if (to->is_unsigned)
        println("fcvtzu x0, d0");
      else
        println("fcvtzs x0, d0");
    }
    return;
  }

  if (is_integer(from) && is_flonum(to)) {
    // x0/w0 → d0
    if (from->size <= 4) {
      if (from->is_unsigned)
        println("ucvtf d0, w0");
      else
        println("scvtf d0, w0");
    } else {
      if (from->is_unsigned)
        println("ucvtf d0, x0");
      else
        println("scvtf d0, x0");
    }
    return;
  }

  // Integer-to-integer cast
  // Don't apply integer truncation/extension to pointer/array/function types
  if (from->base || to->base || from->kind == TY_ARRAY || to->kind == TY_ARRAY ||
      from->kind == TY_PTR || to->kind == TY_PTR)
    return;

  if (to->size == from->size && to->is_unsigned == from->is_unsigned) {
    // Same type, no-op
    return;
  }

  // For widening: sign/zero extend based on SOURCE signedness
  if (to->size > from->size) {
    if (from->size == 1) {
      if (from->is_unsigned) println("and x0, x0, #0xff");
      else println("sxtb x0, w0");
    } else if (from->size == 2) {
      if (from->is_unsigned) println("and x0, x0, #0xffff");
      else println("sxth x0, w0");
    } else if (from->size == 4) {
      if (from->is_unsigned) println("mov w0, w0");
      else println("sxtw x0, w0");
    }
    return;
  }

  // For narrowing or same-size with different signedness: truncate to TARGET
  if (to->size == 1) {
    if (to->is_unsigned) println("and x0, x0, #0xff");
    else println("sxtb x0, w0");
  } else if (to->size == 2) {
    if (to->is_unsigned) println("and x0, x0, #0xffff");
    else println("sxth x0, w0");
  } else if (to->size == 4) {
    if (to->is_unsigned) println("mov w0, w0");
    else
      println("sxtw x0, w0");
  }
  // size == 8: nothing to do
}

// Count number of integer and floating-point parameters (used for ABI)
__attribute__((unused))
static void count_params(Type *func_ty, int *gp_count, int *fp_count) {
  *gp_count = 0;
  *fp_count = 0;
  for (Type *t = func_ty->params; t; t = t->next) {
    if (is_flonum(t))
      (*fp_count)++;
    else
      (*gp_count)++;
  }
}

// Emit code for a function call
// On Apple ARM64:
// - Named args use x0-x7 (GP) and d0-d7 (FP)
// - Variadic args are ALL passed on the stack (not in registers)
// - Stack must be 16-byte aligned before the call
static void gen_funcall(Node *node) {

  Node *args[64];
  int nargs = 0;
  for (Node *arg = node->args; arg; arg = arg->next) {
    args[nargs++] = arg;
    if (nargs >= 64)
      error_tok(node->tok, "too many arguments");
  }

  // Count named vs variadic parameters
  int named_count = 0;
  for (Type *t = node->func_ty->params; t; t = t->next)
    named_count++;

  bool is_variadic = node->func_ty->is_variadic;

  // Determine which args go in registers (named) vs stack (variadic or overflow)
  // Named args: first 8 GP in x0-x7, first 8 FP in d0-d7
  // Variadic args: all on stack
  // Single classification loop: determine where each arg goes
  // (GP register, FP register, or stack) per ARM64 Apple ABI.
  int arg_dest[64]; // >=0: GP reg#, >=100: FP reg#, -1: stack
  int arg_stack_off[64];
  int gp_reg = 0, fp_reg = 0;
  int cur_stack_off = 0;

  for (int i = 0; i < nargs; i++) {
    add_type(args[i]);
    bool is_named_arg = (i < named_count);
    bool is_fp = is_flonum(args[i]->ty);
    Type *aty = args[i]->ty;
    bool is_struct = (aty->kind == TY_STRUCT || aty->kind == TY_UNION || aty->kind == TY_COMPLEX || aty->kind == TY_VECTOR);
    int gp_needed = is_struct ? ((aty->size > 8) ? 2 : 1) : 1;

    if (is_variadic && !is_named_arg) {
      // Apple ARM64: variadic args always go on the stack
      arg_dest[i] = -1;
      arg_stack_off[i] = cur_stack_off;
      cur_stack_off += is_struct ? align_to(aty->size, 8) : 8;
    } else if (is_struct && aty->size > 16 && gp_reg < 8) {
      // Large struct (>16 bytes): passed by indirect reference (pointer in GP reg)
      arg_dest[i] = gp_reg++;
    } else if (is_struct && aty->size <= 16 && gp_reg + gp_needed <= 8) {
      // Small struct: 1-2 GP registers
      arg_dest[i] = gp_reg;
      gp_reg += gp_needed;
    } else if (is_fp && fp_reg < 8) {
      // Float/double: FP register
      arg_dest[i] = 100 + fp_reg++;
    } else if (!is_fp && !is_struct && gp_reg < 8) {
      // Integer/pointer: GP register
      arg_dest[i] = gp_reg++;
    } else {
      // Overflow: stack
      arg_dest[i] = -1;
      arg_stack_off[i] = cur_stack_off;
      cur_stack_off += is_struct ? align_to(aty->size, 8) : 8;
    }
  }

  // Derive stack size from the single classification (single source of truth)
  int padded_stack = align_to(cur_stack_off, 16);

  // Allocate stack for call args FIRST.
  // NOTE: This shifts sp, so any push/pop during arg evaluation will be
  // offset by padded_stack. The push/pop still work correctly because they
  // use sp-relative addressing. The issue is when the CALLER already pushed
  // something before calling gen_funcall — that data is now at sp+padded_stack+depth*16.
  // This is a known limitation.
  if (padded_stack > 0)
    println("sub sp, sp, #%d", padded_stack);

  // Evaluate stack args and store directly to the stack area
  for (int i = 0; i < nargs; i++) {
    if (arg_dest[i] == -1) {
      gen_expr(args[i]);
      Type *aty = args[i]->ty;
      bool is_struct_arg = aty && (aty->kind == TY_STRUCT || aty->kind == TY_UNION || aty->kind == TY_COMPLEX || aty->kind == TY_VECTOR);
      if (is_struct_arg) {
        // x0 is the address of the struct; copy its contents to the stack
        for (int j = 0; j + 7 < aty->size; j += 8) {
          println("ldr x9, [x0, #%d]", j);
          println("str x9, [sp, #%d]", arg_stack_off[i] + j);
        }
        int rem = aty->size % 8;
        int base = aty->size - rem;
        if (rem >= 4) {
          println("ldr w9, [x0, #%d]", base);
          println("str w9, [sp, #%d]", arg_stack_off[i] + base);
          base += 4;
          rem -= 4;
        }
        if (rem >= 2) {
          println("ldrh w9, [x0, #%d]", base);
          println("strh w9, [sp, #%d]", arg_stack_off[i] + base);
          base += 2;
          rem -= 2;
        }
        if (rem >= 1) {
          println("ldrb w9, [x0, #%d]", base);
          println("strb w9, [sp, #%d]", arg_stack_off[i] + base);
        }
      } else if (is_flonum(aty))
        println("str d0, [sp, #%d]", arg_stack_off[i]);
      else
        println("str x0, [sp, #%d]", arg_stack_off[i]);
    }
  }

  // Evaluate register args using push/pop temp stack
  for (int i = 0; i < nargs; i++) {
    if (arg_dest[i] >= 0) {
      gen_expr(args[i]);
      Type *aty = args[i]->ty;
      bool is_struct = aty && (aty->kind == TY_STRUCT || aty->kind == TY_UNION || aty->kind == TY_COMPLEX || aty->kind == TY_VECTOR);

      if (is_struct && aty->size > 16) {
        // Large struct: pass by indirect reference (pointer in GP reg).
        // gen_expr gives us the struct address in x0; pass it directly.
        // The callee will copy it to local storage.
        push();
      } else if (is_struct && aty->size > 8) {
        println("mov x9, x0");
        println("ldr x0, [x9, #8]");
        push();
        println("ldr x0, [x9]");
        push();
      } else if (is_struct) {
        if (aty->size == 1)
          println("ldrb w0, [x0]");
        else if (aty->size == 2)
          println("ldrh w0, [x0]");
        else if (aty->size == 3) {
          println("ldrb w9, [x0, #2]");
          println("ldrh w0, [x0]");
          println("orr w0, w0, w9, lsl #16");
        } else if (aty->size <= 4)
          println("ldr w0, [x0]");
        else if (aty->size == 5) {
          println("ldrb w9, [x0, #4]");
          println("ldr w0, [x0]");
          println("orr x0, x0, x9, lsl #32");
        } else if (aty->size == 6) {
          println("ldrh w9, [x0, #4]");
          println("ldr w0, [x0]");
          println("orr x0, x0, x9, lsl #32");
        } else if (aty->size == 7) {
          println("ldrh w9, [x0, #4]");
          println("ldrb w10, [x0, #6]");
          println("ldr w0, [x0]");
          println("orr x9, x9, x10, lsl #16");
          println("orr x0, x0, x9, lsl #32");
        } else
          println("ldr x0, [x0]");
        push();
      } else if (arg_dest[i] >= 100) {
        pushf();
      } else {
        push();
      }
    }
  }

  // Pop register args in reverse order
  for (int i = nargs - 1; i >= 0; i--) {
    if (arg_dest[i] < 0) continue;
    Type *aty = args[i]->ty;
    bool is_struct = aty && (aty->kind == TY_STRUCT || aty->kind == TY_UNION || aty->kind == TY_COMPLEX || aty->kind == TY_VECTOR);

    if (arg_dest[i] >= 100) {
      popf(arg_dest[i] - 100);
    } else if (is_struct && aty->size > 16) {
      // Large struct: single pointer in one GP register
      pop(argreg64[arg_dest[i]]);
    } else if (is_struct && aty->size > 8) {
      pop(argreg64[arg_dest[i]]);
      pop(argreg64[arg_dest[i] + 1]);
    } else {
      pop(argreg64[arg_dest[i]]);
    }
  }

  // Call the function
  if (node->lhs) {
    // Indirect call through expression (function pointer)
    // Save current register args to stack temporarily
    for (int i = 0; i < gp_reg; i++)
      println("str %s, [sp, #-16]!", argreg64[i]);
    for (int i = 0; i < fp_reg; i++)
      println("str d%d, [sp, #-16]!", i);

    gen_expr(node->lhs);
    println("mov x9, x0");

    // Restore register args
    for (int i = fp_reg - 1; i >= 0; i--)
      println("ldr d%d, [sp], #16", i);
    for (int i = gp_reg - 1; i >= 0; i--)
      println("ldr %s, [sp], #16", argreg64[i]);

    println("blr x9");
  } else {
    println("bl _%s", node->funcname);
  }

  // Clean up stack
  if (padded_stack > 0)
    println("add sp, sp, #%d", padded_stack);

  // Result is in x0 (integer) or d0 (float)
  Type *ret_ty = node->func_ty->return_ty;
  if (ret_ty->kind == TY_BOOL)
    println("and x0, x0, #1");

  // For struct returns: our internal convention passes the address in x0.
  // The caller's store() will copy from this address.
}

//
// Expression code generation
//

static void gen_expr(Node *node) {
  add_type(node);
  // Emit .loc for debugging
  // println(".loc %d %d", node->tok->file->file_no, node->tok->line_no);

  switch (node->kind) {
  case ND_NULL_EXPR:
    return;

  case ND_NUM:
    if (is_flonum(node->ty)) {
      // Load floating-point constant
      union { double d; uint64_t u; } val;
      val.d = node->fval;
      println("mov x0, #%llu", val.u & 0xFFFF);
      if ((val.u >> 16) & 0xFFFF)
        println("movk x0, #%llu, lsl #16", (val.u >> 16) & 0xFFFF);
      if ((val.u >> 32) & 0xFFFF)
        println("movk x0, #%llu, lsl #32", (val.u >> 32) & 0xFFFF);
      if ((val.u >> 48) & 0xFFFF)
        println("movk x0, #%llu, lsl #48", (val.u >> 48) & 0xFFFF);
      println("fmov d0, x0");
      return;
    }

    // Integer constant
    {
      uint64_t val = node->val;
      if (val == 0) {
        println("mov x0, #0");
      } else if (val <= 0xFFFF) {
        println("mov x0, #%llu", (unsigned long long)val);
      } else if ((int64_t)val >= -0x10000 && (int64_t)val < 0) {
        println("mov x0, #%lld", (long long)(int64_t)val);
      } else {
        // Need multiple instructions
        println("mov x0, #%llu", (unsigned long long)(val & 0xFFFF));
        if ((val >> 16) & 0xFFFF)
          println("movk x0, #%llu, lsl #16", (unsigned long long)((val >> 16) & 0xFFFF));
        if ((val >> 32) & 0xFFFF)
          println("movk x0, #%llu, lsl #32", (unsigned long long)((val >> 32) & 0xFFFF));
        if ((val >> 48) & 0xFFFF)
          println("movk x0, #%llu, lsl #48", (unsigned long long)((val >> 48) & 0xFFFF));
      }
    }
    return;

  case ND_NEG:
    gen_expr(node->lhs);
    if (is_flonum(node->ty)) {
      println("fneg d0, d0");
    } else {
      println("neg x0, x0");
    }
    return;

  case ND_REAL:
    // __real__ expr: extract real part of complex
    gen_expr(node->lhs);
    if (is_complex(node->lhs->ty)) {
      load_complex_part(node->lhs->ty->base, 0);
    }
    // If not complex, the value is already in x0/d0
    return;

  case ND_IMAG:
    // __imag__ expr: extract imaginary part of complex
    gen_expr(node->lhs);
    if (is_complex(node->lhs->ty)) {
      load_complex_part(node->lhs->ty->base, node->lhs->ty->base->size);
    } else {
      // Non-complex: imaginary part is 0
      if (is_flonum(node->lhs->ty)) {
        println("movi d0, #0");
      } else {
        println("mov x0, #0");
      }
    }
    return;

  case ND_VLA_PTR: {
    // VLA stack allocation:
    // 1. Load the byte size, round up, subtract from sp
    // 2. Store the new sp as the VLA base address
    //
    // If a saved_sp variable is available (node->lhs), use it
    // for deallocation on re-entry (e.g., goto loops):
    //   - Restore sp from saved_sp if non-zero
    //   - Save current sp to saved_sp before allocating
    Obj *var = node->var;
    Obj *size_var = var->ty->vla_size;

    // Deallocation support: restore/save sp via saved_sp
    if (node->lhs && node->lhs->var) {
      Obj *saved_sp = node->lhs->var;
      int sp_off = -saved_sp->offset;
      if (sp_off <= 4095)
        println("sub x9, x29, #%d", sp_off);
      else {
        load_imm("x9", (uint64_t)sp_off);
        println("sub x9, x29, x9");
      }
      println("ldr x10, [x9]");
      println("cbz x10, .L.vla.skip.%d", label_cnt);
      println("mov sp, x10");
      printlabel(".L.vla.skip.%d:", label_cnt);
      label_cnt++;
      println("mov x10, sp");
      println("str x10, [x9]");
    }

    // Load byte size
    int size_off = -size_var->offset;
    if (size_off <= 4095)
      println("sub x0, x29, #%d", size_off);
    else {
      load_imm("x0", (uint64_t)size_off);
      println("sub x0, x29, x0");
    }
    println("ldr x0, [x0]");

    // Round up to 16-byte alignment
    println("add x0, x0, #15");
    println("and x0, x0, #-16");

    // Subtract from sp to allocate
    println("sub sp, sp, x0");

    // Store the new sp as the VLA base address
    println("mov x0, sp");
    int var_off = -var->offset;
    if (var_off <= 4095)
      println("sub x9, x29, #%d", var_off);
    else {
      load_imm("x9", (uint64_t)var_off);
      println("sub x9, x29, x9");
    }
    println("str x0, [x9]");
    return;
  }

  case ND_VAR:
    gen_addr(node);
    load(node->ty);
    return;

  case ND_MEMBER:
    gen_addr(node);
    // For bitfields, load the underlying storage unit (e.g., long long),
    // not the promoted type (which may be int after integer promotion).
    if (node->member->is_bitfield)
      load(node->member->ty);
    else
      load(node->ty);

    // Handle bitfield
    if (node->member->is_bitfield) {
      int bw = node->member->bit_width;
      int bo = node->member->bit_offset;
      // x0 contains the loaded storage unit (already loaded by load())
      // Extract the bitfield: shift right by bit_offset, mask to bit_width
      if (bo > 0)
        println("lsr x0, x0, #%d", bo);
      if (node->member->ty->is_unsigned || node->member->ty->kind == TY_ENUM) {
        // If bw covers all 64 bits, AND is a no-op (also avoids invalid immediate)
        if (bw < 64)
          println("and x0, x0, #%llu", (unsigned long long)((1ULL << bw) - 1));
      } else {
        // Sign extend: shift left then arithmetic shift right
        int shift = 64 - bw;
        println("lsl x0, x0, #%d", shift);
        println("asr x0, x0, #%d", shift);
      }
    }
    return;

  case ND_DEREF:
    gen_expr(node->lhs);
    load(node->ty);
    return;

  case ND_ADDR:
    gen_addr(node->lhs);
    return;

  case ND_ASSIGN:
    gen_addr(node->lhs);
    push();
    gen_expr(node->rhs);

    if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield) {
      // Bitfield assignment
      int bw = node->lhs->member->bit_width;
      int bo = node->lhs->member->bit_offset;
      int sz = node->lhs->member->ty->size; // storage unit size (1, 2, 4, or 8)
      bool is64 = (sz == 8);
      char *wr = is64 ? "x" : "w"; // register prefix

      println("mov x10, x0");  // save new value
      pop("x1"); // struct member address

      // Load old storage unit value
      if (sz == 1) println("ldrb w2, [x1]");
      else if (sz == 2) println("ldrh w2, [x1]");
      else if (sz == 4) println("ldr w2, [x1]");
      else println("ldr x2, [x1]");

      // Clear the bitfield bits in old value
      uint64_t field_mask = ((1ULL << bw) - 1) << bo;
      uint64_t unit_mask = (sz == 8) ? ~0ULL : (sz == 4) ? 0xFFFFFFFFULL : (sz == 2) ? 0xFFFFULL : 0xFFULL;
      uint64_t clear_mask = ~field_mask & unit_mask;
      if (is64) {
        println("mov x3, #%llu", (unsigned long long)(clear_mask & 0xFFFF));
        if ((clear_mask >> 16) & 0xFFFF)
          println("movk x3, #%llu, lsl #16", (unsigned long long)((clear_mask >> 16) & 0xFFFF));
        if ((clear_mask >> 32) & 0xFFFF)
          println("movk x3, #%llu, lsl #32", (unsigned long long)((clear_mask >> 32) & 0xFFFF));
        if ((clear_mask >> 48) & 0xFFFF)
          println("movk x3, #%llu, lsl #48", (unsigned long long)((clear_mask >> 48) & 0xFFFF));
        println("and x2, x2, x3");
      } else {
        println("mov w3, #%u", (unsigned)(clear_mask & 0xFFFF));
        if ((clear_mask >> 16) & 0xFFFF)
          println("movk w3, #%u, lsl #16", (unsigned)((clear_mask >> 16) & 0xFFFF));
        println("and w2, w2, w3");
      }

      // Mask and shift new value, OR into old
      uint64_t val_mask = (1ULL << bw) - 1;
      if (is64) {
        println("mov x3, #%llu", (unsigned long long)(val_mask & 0xFFFF));
        if ((val_mask >> 16) & 0xFFFF)
          println("movk x3, #%llu, lsl #16", (unsigned long long)((val_mask >> 16) & 0xFFFF));
        if ((val_mask >> 32) & 0xFFFF)
          println("movk x3, #%llu, lsl #32", (unsigned long long)((val_mask >> 32) & 0xFFFF));
        if ((val_mask >> 48) & 0xFFFF)
          println("movk x3, #%llu, lsl #48", (unsigned long long)((val_mask >> 48) & 0xFFFF));
        println("and x0, x10, x3");
      } else {
        // If val_mask covers all bits for the container size, AND is a no-op
        if ((unsigned)(val_mask & 0xFFFFFFFF) == 0xFFFFFFFF)
          println("mov w0, w10");
        else
          println("and w0, w10, #%u", (unsigned)(val_mask & 0xFFFFFFFF));
      }
      if (bo > 0)
        println("lsl %s0, %s0, #%d", wr, wr, bo);
      println("orr %s2, %s2, %s0", wr, wr, wr);

      // Store back
      if (sz == 1) println("strb w2, [x1]");
      else if (sz == 2) println("strh w2, [x1]");
      else if (sz == 4) println("str w2, [x1]");
      else println("str x2, [x1]");

      // Return the truncated (and sign-extended) value that was actually stored.
      // x10 holds the original unmasked value; mask it to bw bits.
      if (node->lhs->member->ty->is_unsigned || node->lhs->member->ty->kind == TY_ENUM) {
        if (bw < 64)
          println("and x0, x10, #%llu", (unsigned long long)((1ULL << bw) - 1));
        else
          println("mov x0, x10");
      } else {
        // Signed: mask then sign-extend via shift left + arithmetic shift right
        int shift = 64 - bw;
        println("lsl x0, x10, #%d", shift);
        println("asr x0, x0, #%d", shift);
      }
      return;
    }

    store(node->ty);
    return;

  case ND_STMT_EXPR:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;

  case ND_COMMA:
    gen_expr(node->lhs);
    gen_expr(node->rhs);
    return;

  case ND_CAST:
    gen_expr(node->lhs);
    cast(node->lhs->ty, node->ty);
    return;

  case ND_MEMZERO: {
    // Zero-fill a local variable
    int sz = node->var->ty->size;
    int off = node->var->offset; // negative offset from x29

    // Compute base address
    if (off < 0 && -off <= 4095) {
      println("sub x1, x29, #%d", -off);
    } else if (off < 0) {
      load_imm("x1", (uint64_t)(-off));
      println("sub x1, x29, x1");
    } else if (off <= 4095) {
      println("add x1, x29, #%d", off);
    } else {
      load_imm("x1", (uint64_t)off);
      println("add x1, x29, x1");
    }

    println("mov x2, #0");
    int zeroed = 0;
    int zoff = 0;
    while (zeroed + 8 <= sz) {
      if (zoff + 8 > 32760) {
        // Advance base pointer to keep offsets in range for str x
        println("add x1, x1, #%d", zoff);
        zoff = 0;
      }
      println("str x2, [x1, #%d]", zoff);
      zoff += 8;
      zeroed += 8;
    }
    // Handle remaining bytes (< 8)
    while (zeroed < sz) {
      if (zoff >= 4096) {
        // Advance base pointer to keep offsets in range for strb
        println("add x1, x1, #%d", zoff);
        zoff = 0;
      }
      println("strb w2, [x1, #%d]", zoff);
      zoff++;
      zeroed++;
    }
    return;
  }

  case ND_COND: {
    int c = label_cnt++;
    gen_cond(node->cond);
    println("b.eq .L.else.%d", c);
    gen_expr(node->then);
    println("b .L.end.%d", c);
    printlabel(".L.else.%d:", c);
    gen_expr(node->els);
    printlabel(".L.end.%d:", c);
    return;
  }

  case ND_NOT:
    gen_expr(node->lhs);
    add_type(node->lhs);
    if (is_flonum(node->lhs->ty)) {
      println("fcmp d0, #0.0");
      println("cset w0, eq");
    } else {
      println("cmp x0, #0");
      println("cset w0, eq");
    }
    return;

  case ND_BITNOT:
    gen_expr(node->lhs);
    if (node->ty->size <= 4)
      println("mvn w0, w0");
    else
      println("mvn x0, x0");
    return;

  case ND_LOGAND: {
    int c = label_cnt++;
    gen_cond(node->lhs);
    println("b.eq .L.false.%d", c);
    gen_cond(node->rhs);
    println("b.eq .L.false.%d", c);
    println("mov x0, #1");
    println("b .L.end.%d", c);
    printlabel(".L.false.%d:", c);
    println("mov x0, #0");
    printlabel(".L.end.%d:", c);
    return;
  }

  case ND_LOGOR: {
    int c = label_cnt++;
    gen_cond(node->lhs);
    println("b.ne .L.true.%d", c);
    gen_cond(node->rhs);
    println("b.ne .L.true.%d", c);
    println("mov x0, #0");
    println("b .L.end.%d", c);
    printlabel(".L.true.%d:", c);
    println("mov x0, #1");
    printlabel(".L.end.%d:", c);
    return;
  }

  case ND_FUNCALL:
    gen_funcall(node);
    return;

  case ND_FRAME_ADDR:
    println("mov x0, x29");
    return;

  case ND_RETURN_ADDR: {
    // __builtin_return_address(level)
    // Level 0: return address of current function = [x29, #8]
    // Level N: follow frame pointer chain N times, then read [fp, #8]
    gen_expr(node->lhs);
    int c = label_cnt++;
    println("mov x1, x29"); // start from current frame
    println("cbz x0, .L.ra.done.%d", c);
    printlabel(".L.ra.loop.%d:", c);
    println("ldr x1, [x1]"); // follow frame pointer chain
    println("sub x0, x0, #1");
    println("cbnz x0, .L.ra.loop.%d", c);
    printlabel(".L.ra.done.%d:", c);
    println("ldr x0, [x1, #8]"); // return address is at fp+8
    return;
  }

  case ND_BUILTIN_FRAME_ADDR: {
    // __builtin_frame_address(level)
    // Level 0: x29
    // Level N: follow frame pointer chain N times
    gen_expr(node->lhs);
    int c = label_cnt++;
    println("mov x1, x29");
    println("cbz x0, .L.fa.done.%d", c);
    printlabel(".L.fa.loop.%d:", c);
    println("ldr x1, [x1]"); // follow frame pointer chain
    println("sub x0, x0, #1");
    println("cbnz x0, .L.fa.loop.%d", c);
    printlabel(".L.fa.done.%d:", c);
    println("mov x0, x1");
    return;
  }

  case ND_BUILTIN_SETJMP: {
    // __builtin_setjmp(buf):
    // buf[0] = fp (x29)
    // buf[1] = sp
    // buf[2] = return point (address of label after setjmp)
    // Returns 0 on first call, 1 on longjmp return
    gen_expr(node->lhs);  // buf address in x0
    println("str x29, [x0]");       // buf[0] = fp
    println("mov x1, sp");
    println("str x1, [x0, #8]");    // buf[1] = sp
    int c = label_cnt++;
    println("adr x1, .L.sjret.%d", c);
    println("str x1, [x0, #16]");   // buf[2] = return label
    println("mov x0, #0");          // first call returns 0
    println("b .L.sjdone.%d", c);
    printlabel(".L.sjret.%d:", c);
    println("mov x0, #1");          // longjmp return returns 1
    printlabel(".L.sjdone.%d:", c);
    return;
  }

  case ND_BUILTIN_LONGJMP: {
    // __builtin_longjmp(buf, val):
    // Restore fp, sp, and jump to the return point
    gen_expr(node->lhs);  // buf address in x0
    println("ldr x29, [x0]");       // restore fp
    println("ldr x1, [x0, #8]");    // restore sp
    println("mov sp, x1");
    println("ldr x1, [x0, #16]");   // return point
    println("br x1");                // jump to return point
    return;
  }

  case ND_BUILTIN_ADD_OVERFLOW:
  case ND_BUILTIN_SUB_OVERFLOW:
  case ND_BUILTIN_MUL_OVERFLOW: {
    // Evaluate a and b
    gen_expr(node->lhs);
    push();  // push a (x0)
    gen_expr(node->rhs);
    println("mov x1, x0");
    pop("x0");  // pop a into x0, b is in x1

    Type *rty = node->overflow_ty;
    bool is_unsigned = rty->is_unsigned;
    int sz = rty->size;

    if (node->kind == ND_BUILTIN_ADD_OVERFLOW) {
      if (is_unsigned) {
        if (sz <= 4) {
          println("add w2, w0, w1");   // result in w2
          println("cmp w2, w0");       // if result < a, overflow
          println("cset w3, lo");      // overflow flag
        } else {
          println("adds x2, x0, x1");
          println("cset w3, cs");
        }
      } else {
        if (sz <= 4) {
          println("adds w2, w0, w1");
          println("cset w3, vs");
        } else {
          println("adds x2, x0, x1");
          println("cset w3, vs");
        }
      }
    } else if (node->kind == ND_BUILTIN_SUB_OVERFLOW) {
      if (is_unsigned) {
        if (sz <= 4) {
          println("subs w2, w0, w1");
          println("cset w3, lo");
        } else {
          println("subs x2, x0, x1");
          println("cset w3, lo");
        }
      } else {
        if (sz <= 4) {
          println("subs w2, w0, w1");
          println("cset w3, vs");
        } else {
          println("subs x2, x0, x1");
          println("cset w3, vs");
        }
      }
    } else { // MUL
      if (is_unsigned) {
        if (sz <= 4) {
          println("umull x2, w0, w1");
          println("lsr x4, x2, #32");
          println("cmp x4, #0");
          println("cset w3, ne");
        } else {
          println("umulh x4, x0, x1");
          println("mul x2, x0, x1");
          println("cmp x4, #0");
          println("cset w3, ne");
        }
      } else {
        if (sz <= 4) {
          println("smull x2, w0, w1");
          println("cmp x2, w2, sxtw");
          println("cset w3, ne");
        } else {
          println("mul x2, x0, x1");
          println("smulh x4, x0, x1");
          println("cmp x4, x2, asr #63");
          println("cset w3, ne");
        }
      }
    }

    // Store result if result pointer is provided (non-_p variant)
    if (node->args) {
      // Save result and overflow flag, then evaluate the result pointer
      println("stp x2, x3, [sp, #-16]!");
      depth++;
      gen_expr(node->args);
      // x0 = result_ptr, restore x2=result, x3=overflow
      println("ldp x2, x3, [sp], #16");
      depth--;
      // Store w2/x2 to [x0]
      switch (sz) {
      case 1: println("strb w2, [x0]"); break;
      case 2: println("strh w2, [x0]"); break;
      case 4: println("str w2, [x0]"); break;
      case 8: println("str x2, [x0]"); break;
      }
      println("mov w0, w3");
    } else {
      println("mov w0, w3");
    }
    return;
  }

  case ND_CHAIN_VAR:
    gen_addr(node);
    load(node->ty);
    return;

  case ND_LABEL_VAL: {
    // Computed goto label address. Use adrp+add for non-local symbols
    // since adr doesn't support cross-section relocations on Mach-O.
    if (!strncmp(node->unique_label, "_cg_", 4)) {
      println("adrp x0, %s@PAGE", node->unique_label);
      println("add x0, x0, %s@PAGEOFF", node->unique_label);
    } else {
      println("adr x0, %s", node->unique_label);
    }
    return;
  }

  case ND_CAS: {
    // Atomic compare-and-swap
    gen_expr(node->cas_addr);
    push();
    gen_expr(node->cas_new);
    push();
    gen_expr(node->cas_old);
    println("mov x3, x0"); // old val ptr
    pop("x2");  // new val
    pop("x1");  // addr
    println("ldr x4, [x3]"); // expected old val
    int c = label_cnt++;
    printlabel(".L.cas.%d:", c);
    println("ldaxr x0, [x1]");
    println("cmp x0, x4");
    println("b.ne .L.cas.fail.%d", c);
    println("stlxr w5, x2, [x1]");
    println("cbnz w5, .L.cas.%d", c);
    println("str x0, [x3]");
    println("mov x0, #1");
    println("b .L.cas.end.%d", c);
    printlabel(".L.cas.fail.%d:", c);
    println("str x0, [x3]");
    println("mov x0, #0");
    printlabel(".L.cas.end.%d:", c);
    return;
  }

  case ND_EXCH: {
    gen_expr(node->cas_addr);
    push();
    gen_expr(node->cas_new);
    pop("x1");
    int c = label_cnt++;
    printlabel(".L.exch.%d:", c);
    println("ldaxr x2, [x1]");
    println("stlxr w3, x0, [x1]");
    println("cbnz w3, .L.exch.%d", c);
    println("mov x0, x2");
    return;
  }

  case ND_BUILTIN_CLZ: {
    // count leading zeros: CLZ instruction on ARM64
    gen_expr(node->lhs);
    if (node->val) // 64-bit variant
      println("clz x0, x0");
    else
      println("clz w0, w0");
    return;
  }

  case ND_BUILTIN_CTZ: {
    // count trailing zeros: RBIT + CLZ
    gen_expr(node->lhs);
    if (node->val) { // 64-bit
      println("rbit x0, x0");
      println("clz x0, x0");
    } else {
      println("rbit w0, w0");
      println("clz w0, w0");
    }
    return;
  }

  case ND_BUILTIN_FFS: {
    // find first set: if x==0 return 0, else ctz(x)+1
    gen_expr(node->lhs);
    int c = label_cnt++;
    if (node->val) { // 64-bit
      println("cmp x0, #0");
      println("b.eq .L.ffs.zero.%d", c);
      println("rbit x0, x0");
      println("clz x0, x0");
      println("add w0, w0, #1");
    } else {
      println("cmp w0, #0");
      println("b.eq .L.ffs.zero.%d", c);
      println("rbit w0, w0");
      println("clz w0, w0");
      println("add w0, w0, #1");
    }
    println("b .L.ffs.end.%d", c);
    printlabel(".L.ffs.zero.%d:", c);
    println("mov w0, #0");
    printlabel(".L.ffs.end.%d:", c);
    return;
  }

  case ND_BUILTIN_POPCOUNT: {
    // Population count using NEON: fmov d0,x0; cnt v0.8b,v0.8b; addv b0,v0.8b; fmov w0,s0
    gen_expr(node->lhs);
    println("fmov d0, x0");
    println("cnt v0.8b, v0.8b");
    println("addv b0, v0.8b");
    println("fmov w0, s0");
    return;
  }

  case ND_BUILTIN_PARITY: {
    // parity = popcount(x) & 1
    gen_expr(node->lhs);
    println("fmov d0, x0");
    println("cnt v0.8b, v0.8b");
    println("addv b0, v0.8b");
    println("fmov w0, s0");
    println("and w0, w0, #1");
    return;
  }

  case ND_BUILTIN_CLRSB: {
    // cls instruction on ARM64 counts leading redundant sign bits
    gen_expr(node->lhs);
    if (node->val) // 64-bit
      println("cls x0, x0");
    else
      println("cls w0, w0");
    return;
  }

  case ND_BUILTIN_BSWAP32: {
    gen_expr(node->lhs);
    if (node->val == 16) {
      // bswap16: reverse bytes in halfword
      println("rev w0, w0");
      println("lsr w0, w0, #16");
    } else {
      println("rev w0, w0");
    }
    return;
  }

  case ND_BUILTIN_BSWAP64: {
    gen_expr(node->lhs);
    println("rev x0, x0");
    return;
  }

  case ND_BUILTIN_ALLOCA: {
    // __builtin_alloca(size): allocate on stack, aligned to 16 bytes.
    // We lower sp by (size + expr_stack_size), copy expression stack
    // items down, and return the alloca'd address.

    // 1. Evaluate size into x0
    gen_expr(node->lhs);
    // 2. Round up to 16-byte alignment
    println("add x0, x0, #15");
    println("and x0, x0, #-16");
    // Save aligned size in x10
    println("mov x10, x0");

    // 3. Save old sp
    println("mov x11, sp");

    // 4. New sp = old sp - alloca_size
    println("sub sp, sp, x10");

    // 5. Copy expression stack items down (they were at old sp)
    //    depth items, each 16 bytes, starting from old sp
    for (int i = 0; i < depth; i++) {
      println("ldr x12, [x11, #%d]", i * 16);
      println("str x12, [sp, #%d]", i * 16);
    }

    // 6. Update alloca_bottom
    {
      int off = current_fn->alloca_bottom->offset;
      if (off < 0 && -off <= 4095)
        println("sub x9, x29, #%d", -off);
      else if (off < 0) {
        load_imm("x9", (uint64_t)(-off));
        println("sub x9, x29, x9");
      } else if (off <= 4095)
        println("add x9, x29, #%d", off);
      else {
        load_imm("x9", (uint64_t)off);
        println("add x9, x29, x9");
      }
    }
    // alloca_bottom = sp + depth*16 (bottom of alloca'd area)
    if (depth > 0) {
      println("add x0, sp, #%d", depth * 16);
      println("str x0, [x9]");
    } else {
      println("mov x0, sp");
      println("str x0, [x9]");
    }

    // 7. The alloca'd memory is at [old sp - alloca_size .. old sp)
    //    Which is [sp + depth*16 .. sp + depth*16 + alloca_size)
    //    But the caller wants the start address of the alloca'd block.
    //    old sp - alloca_size = new sp + depth*16... no.
    //    old sp = x11. alloca'd memory starts at x11 - alloca_size.
    //    new sp = x11 - alloca_size. So alloca'd memory starts at new sp.
    //    But expression stack items were copied to [new sp..new sp+depth*16).
    //    So alloca'd memory is actually at [new sp + depth*16 .. old sp)
    //    = [sp + depth*16 .. x11)
    if (depth > 0)
      println("add x0, sp, #%d", depth * 16);
    else
      println("mov x0, sp");
    return;
  }

  default:
    break;
  }

  // Binary operations

  // Vector binary/unary ops are decomposed into element-wise scalar ops
  // in the parser. They should not reach codegen as vector-typed binary nodes.

  // Float binary ops
  if (is_flonum(node->lhs->ty)) {
    gen_expr(node->rhs);
    pushf();
    gen_expr(node->lhs);
    popf(1);

    switch (node->kind) {
    case ND_ADD: println("fadd d0, d0, d1"); return;
    case ND_SUB: println("fsub d0, d0, d1"); return;
    case ND_MUL: println("fmul d0, d0, d1"); return;
    case ND_DIV: println("fdiv d0, d0, d1"); return;
    case ND_EQ:
      println("fcmp d0, d1");
      println("cset w0, eq");
      return;
    case ND_NE:
      println("fcmp d0, d1");
      println("cset w0, ne");
      return;
    case ND_LT:
      println("fcmp d0, d1");
      println("cset w0, mi");
      return;
    case ND_LE:
      println("fcmp d0, d1");
      println("cset w0, ls");
      return;
    default:
      error_tok(node->tok, "invalid expression");
    }
  }

  // Integer binary ops
  gen_expr(node->rhs);
  push();
  gen_expr(node->lhs);
  pop("x1");

  char *rd = "x0", *rn = "x0", *rm = "x1";
  bool is_long = node->lhs->ty->size == 8 || node->lhs->ty->base;

  if (!is_long) {
    rd = "w0"; rn = "w0"; rm = "w1";
  }

  switch (node->kind) {
  case ND_ADD:
    println("add %s, %s, %s", rd, rn, rm);
    return;
  case ND_SUB:
    println("sub %s, %s, %s", rd, rn, rm);
    return;
  case ND_MUL:
    println("mul %s, %s, %s", rd, rn, rm);
    return;
  case ND_DIV:
    if (node->ty->is_unsigned)
      println("udiv %s, %s, %s", rd, rn, rm);
    else
      println("sdiv %s, %s, %s", rd, rn, rm);
    return;
  case ND_MOD:
    // x0 = lhs, x1 = rhs → x0 = lhs % rhs
    // sdiv x9, x0, x1; msub x0, x9, x1, x0
    if (is_long) {
      if (node->ty->is_unsigned)
        println("udiv x9, x0, x1");
      else
        println("sdiv x9, x0, x1");
      println("msub x0, x9, x1, x0");
    } else {
      if (node->ty->is_unsigned)
        println("udiv w9, w0, w1");
      else
        println("sdiv w9, w0, w1");
      println("msub w0, w9, w1, w0");
    }
    return;
  case ND_BITAND:
    println("and %s, %s, %s", rd, rn, rm);
    return;
  case ND_BITOR:
    println("orr %s, %s, %s", rd, rn, rm);
    return;
  case ND_BITXOR:
    println("eor %s, %s, %s", rd, rn, rm);
    return;
  case ND_SHL:
    println("lsl %s, %s, %s", rd, rn, rm);
    return;
  case ND_SHR:
    if (node->lhs->ty->is_unsigned)
      println("lsr %s, %s, %s", rd, rn, rm);
    else
      println("asr %s, %s, %s", rd, rn, rm);
    return;
  case ND_EQ:
    println("cmp %s, %s", rn, rm);
    println("cset w0, eq");
    return;
  case ND_NE:
    println("cmp %s, %s", rn, rm);
    println("cset w0, ne");
    return;
  case ND_LT:
    println("cmp %s, %s", rn, rm);
    if (node->lhs->ty->is_unsigned)
      println("cset w0, lo");
    else
      println("cset w0, lt");
    return;
  case ND_LE:
    println("cmp %s, %s", rn, rm);
    if (node->lhs->ty->is_unsigned)
      println("cset w0, ls");
    else
      println("cset w0, le");
    return;
  default:
    error_tok(node->tok, "invalid expression");
  }
}

//
// Statement code generation
//

// Walk a dead code subtree emitting only case/default labels (for switch).
// Everything else is skipped. Once a case label is found, its body is
// generated normally (it's reachable via the switch dispatch).
static void gen_stmt_dead(Node *node) {
  if (!node) return;
  switch (node->kind) {
  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt_dead(n);
    return;
  case ND_CASE:
    // Case/default label — emit it and switch to normal codegen
    printlabel("%s:", node->label);
    gen_stmt(node->lhs);
    return;
  case ND_IF:
    gen_stmt_dead(node->then);
    if (node->els)
      gen_stmt_dead(node->els);
    return;
  case ND_FOR:
  case ND_DO:
    gen_stmt_dead(node->then);
    return;
  default:
    return;  // skip dead code
  }
}

static void gen_stmt(Node *node) {
  add_type(node);
  switch (node->kind) {
  case ND_IF: {
    // Constant condition optimization: avoid emitting dead code that
    // references undefined symbols. Case labels inside dead branches
    // are still emitted (they're reachable via switch dispatch).
    if (node->cond->kind == ND_NUM && is_integer(node->cond->ty)) {
      if (node->cond->val == 0) {
        int c = label_cnt++;
        println("b .L.end.%d", c);
        gen_stmt_dead(node->then);
        printlabel(".L.end.%d:", c);
        if (node->els)
          gen_stmt(node->els);
        return;
      } else {
        gen_stmt(node->then);
        return;
      }
    }
    int c = label_cnt++;
    gen_cond(node->cond);
    println("b.eq .L.else.%d", c);
    gen_stmt(node->then);
    println("b .L.end.%d", c);
    printlabel(".L.else.%d:", c);
    if (node->els)
      gen_stmt(node->els);
    printlabel(".L.end.%d:", c);
    return;
  }

  case ND_FOR: {
    int c = label_cnt++;
    if (node->init)
      gen_stmt(node->init);
    printlabel(".L.begin.%d:", c);
    if (node->cond) {
      gen_cond(node->cond);
      println("b.eq %s", node->unique_label);
    }
    gen_stmt(node->then);
    if (node->cont_label)
      printlabel("%s:", node->cont_label);
    if (node->inc)
      gen_expr(node->inc);
    println("b .L.begin.%d", c);
    printlabel("%s:", node->unique_label);
    return;
  }

  case ND_DO: {
    int c = label_cnt++;
    printlabel(".L.begin.%d:", c);
    gen_stmt(node->then);
    if (node->cont_label)
      printlabel("%s:", node->cont_label);
    gen_cond(node->cond);
    println("b.ne .L.begin.%d", c);
    printlabel("%s:", node->unique_label);
    return;
  }

  case ND_SWITCH:
    gen_expr(node->cond);

    for (Node *n = node->case_next; n; n = n->case_next) {
      if (n->begin == n->end) {
        if (n->begin >= 0 && n->begin <= 4095) {
          println("cmp x0, #%ld", n->begin);
        } else {
          // Load large immediate via movz/movk
          uint64_t v = (uint64_t)n->begin;
          println("movz x9, #%llu", (unsigned long long)(v & 0xFFFF));
          if ((v >> 16) & 0xFFFF)
            println("movk x9, #%llu, lsl #16", (unsigned long long)((v >> 16) & 0xFFFF));
          if ((v >> 32) & 0xFFFF)
            println("movk x9, #%llu, lsl #32", (unsigned long long)((v >> 32) & 0xFFFF));
          if ((v >> 48) & 0xFFFF)
            println("movk x9, #%llu, lsl #48", (unsigned long long)((v >> 48) & 0xFFFF));
          println("cmp x0, x9");
        }
        println("b.eq %s", n->label);
      } else {
        // Case range
        load_imm("x9", (uint64_t)(int64_t)n->begin);
        println("cmp x0, x9");
        println("b.lt .L.next.%s", n->label);
        load_imm("x9", (uint64_t)(int64_t)n->end);
        println("cmp x0, x9");
        println("b.le %s", n->label);
        printlabel(".L.next.%s:", n->label);
      }
    }

    if (node->default_case)
      println("b %s", node->default_case->label);
    println("b %s", node->unique_label);
    gen_stmt(node->then);
    printlabel("%s:", node->unique_label);
    return;

  case ND_CASE:
    printlabel("%s:", node->label);
    gen_stmt(node->lhs);
    return;

  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;

  case ND_GOTO:
    if (node->nlgoto_buf) {
      // Non-local goto: longjmp via buf accessed through chain
      gen_addr(node->lhs);  // buf address → x0
      println("ldr x29, [x0]");       // restore fp
      println("ldr x1, [x0, #8]");    // restore sp
      println("mov sp, x1");
      println("ldr x1, [x0, #16]");   // label address
      println("br x1");
    } else {
      println("b %s", node->unique_label);
    }
    return;

  case ND_GOTO_EXPR:
    gen_expr(node->lhs);
    println("br x0");
    return;

  case ND_LABEL:
    printlabel("%s:", node->unique_label);
    gen_stmt(node->lhs);
    return;

  case ND_RETURN:
    if (node->lhs) {
      gen_expr(node->lhs);
      if (node->lhs->ty && is_flonum(node->lhs->ty)) {
        // Result already in d0
      }
      // For struct returns, x0 has the address of the struct data.
      // The caller will copy from this address.
    }
    println("b .L.return.%s", current_fn->name);
    return;

  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    return;

  case ND_ASM: {
    // If no operands, just emit the template as before
    if (node->asm_num_outputs == 0 && node->asm_num_inputs == 0) {
      if (node->asm_str[0] != '\0')
        println("%s", node->asm_str);
      return;
    }

    // Inline asm with register constraints.
    // We use callee-saved registers x19-x28 for operands so they
    // don't conflict with the compiler's use of x0-x7.
    // Operand numbering: outputs 0..N-1, then inputs N..N+M-1.
    int total = node->asm_num_outputs + node->asm_num_inputs;
    if (total > 10)
      error_tok(node->tok, "too many asm operands (max 10)");

    // reg_name[i] = register name for operand i
    char *reg_name[10];
    // For outputs we may need the address to store back to
    // addr_saved[i] = 1 if we saved the address on the stack for output i
    int addr_on_stack[10] = {0};

    // --- Phase 1: Assign registers to each operand ---
    // First pass: assign registers for outputs, determine tied inputs
    for (int i = 0; i < node->asm_num_outputs; i++)
      reg_name[i] = format("x%d", 19 + i);

    for (int i = 0; i < node->asm_num_inputs; i++) {
      char *c = node->asm_input_constraints[i];
      if (c[0] >= '0' && c[0] <= '9') {
        // Tied operand: use the same register as the referenced output
        int tied = c[0] - '0';
        if (tied >= node->asm_num_outputs)
          error_tok(node->tok, "asm tied operand out of range");
        reg_name[node->asm_num_outputs + i] = reg_name[tied];
      } else {
        reg_name[node->asm_num_outputs + i] = format("x%d", 19 + node->asm_num_outputs + i);
      }
    }

    // --- Phase 2: Save callee-saved registers we'll use ---
    // x19-x28 are callee-saved, so the function prologue/epilogue
    // should already save/restore them. We just need to be careful
    // not to clobber values the compiler put there. For safety,
    // save and restore any registers we use.
    // Collect unique registers used.
    int regs_used[10] = {0}; // register numbers (19-28)
    int num_regs_used = 0;
    for (int i = 0; i < total; i++) {
      int rn;
      if (sscanf(reg_name[i], "x%d", &rn) == 1) {
        bool found = false;
        for (int j = 0; j < num_regs_used; j++)
          if (regs_used[j] == rn) { found = true; break; }
        if (!found)
          regs_used[num_regs_used++] = rn;
      }
    }
    for (int i = 0; i < num_regs_used; i++)
      println("str x%d, [sp, #-16]!", regs_used[i]);

    // --- Phase 3: Evaluate output operands ---
    // For "+r" (read-write): evaluate address, load current value into reg
    // For "=r" (write-only): evaluate address, save it for later store
    for (int i = 0; i < node->asm_num_outputs; i++) {
      char *c = node->asm_output_constraints[i];
      bool is_rw = (strchr(c, '+') != NULL); // "+r"
      // Evaluate the output lvalue to get its address
      gen_addr(node->asm_output_exprs[i]);
      // Save the address on the stack for later store-back
      println("str x0, [sp, #-16]!");
      depth++;
      addr_on_stack[i] = 1;

      if (is_rw) {
        // Read-modify-write: load the current value into the register
        // x0 still has the address, but we just pushed it. Peek from stack.
        println("ldr x0, [sp]"); // peek the address
        // Load the value according to the expression's type
        Type *ty = node->asm_output_exprs[i]->ty;
        if (ty->size == 1)
          println(ty->is_unsigned ? "ldrb w0, [x0]" : "ldrsb w0, [x0]");
        else if (ty->size == 2)
          println(ty->is_unsigned ? "ldrh w0, [x0]" : "ldrsh w0, [x0]");
        else if (ty->size == 4) {
          println("ldr w0, [x0]");
          if (!ty->is_unsigned)
            println("sxtw x0, w0");
        } else
          println("ldr x0, [x0]");
        println("mov %s, x0", reg_name[i]);
      }
    }

    // --- Phase 4: Evaluate input operands ---
    for (int i = 0; i < node->asm_num_inputs; i++) {
      int idx = node->asm_num_outputs + i;
      char *c = node->asm_input_constraints[i];
      if (c[0] >= '0' && c[0] <= '9') {
        // Tied operand: load value into the shared register
        gen_expr(node->asm_input_exprs[i]);
        println("mov %s, x0", reg_name[idx]);
      } else {
        // "r" constraint: load value into the assigned register
        gen_expr(node->asm_input_exprs[i]);
        println("mov %s, x0", reg_name[idx]);
      }
    }

    // --- Phase 5: Emit the asm template ---
    // Substitute %0, %1, etc. with register names
    if (node->asm_str[0] != '\0') {
      char buf[4096];
      int bi = 0;
      for (char *p = node->asm_str; *p; p++) {
        if (*p == '%' && p[1] == '%') {
          buf[bi++] = '%';
          p++;
        } else if (*p == '%' && p[1] >= '0' && p[1] <= '9') {
          int operand_num = p[1] - '0';
          if (operand_num >= total)
            error_tok(node->tok, "asm operand %%%d out of range", operand_num);
          char *rn = reg_name[operand_num];
          for (char *r = rn; *r; r++)
            buf[bi++] = *r;
          p++;
        } else {
          buf[bi++] = *p;
        }
      }
      buf[bi] = '\0';
      println("%s", buf);
    }

    // --- Phase 6: Store output operands back ---
    // Pop addresses in reverse order and store register values
    for (int i = node->asm_num_outputs - 1; i >= 0; i--) {
      if (addr_on_stack[i]) {
        char *c = node->asm_output_constraints[i];
        // For pure memory constraints ("=m", "+m"), the asm operates on memory
        // directly; do NOT store back from a register (it may hold garbage).
        bool is_mem_only = (strchr(c, 'm') != NULL && strchr(c, 'r') == NULL);
        println("ldr x0, [sp], #16"); // pop address
        depth--;
        if (!is_mem_only) {
          // Store the register value to the address
          Type *ty = node->asm_output_exprs[i]->ty;
          if (ty->size == 1)
            println("strb w%d, [x0]", 19 + i);
          else if (ty->size == 2)
            println("strh w%d, [x0]", 19 + i);
          else if (ty->size == 4)
            println("str w%d, [x0]", 19 + i);
          else
            println("str x%d, [x0]", 19 + i);
        }
      }
    }

    // --- Phase 7: Restore callee-saved registers ---
    for (int i = num_regs_used - 1; i >= 0; i--)
      println("ldr x%d, [sp], #16", regs_used[i]);

    return;
  }

  default:
    // Treat unknown node kinds as expression statements
    // (e.g., ND_COMMA from initializers, ND_ASSIGN, etc.)
    gen_expr(node);
    return;
  }
}

//
// Assign offsets to local variables
//

static void assign_lvar_offsets(Obj *fn) {
  // Params beyond registers come from caller's stack (positive offsets from x29)
  int stack_param_offset = 16; // x29+16 is first caller stack arg
  int gp2 = 0, fp2 = 0;
  for (Obj *param = fn->params; param; param = param->next) {
    bool is_fp = is_flonum(param->ty);
    bool is_struct = (param->ty->kind == TY_STRUCT || param->ty->kind == TY_UNION || param->ty->kind == TY_COMPLEX || param->ty->kind == TY_VECTOR);

    int gp_needed = is_struct ? ((param->ty->size > 16) ? 1 : (param->ty->size > 8) ? 2 : 1) : 1;

    if (is_fp && fp2 < 8) {
      fp2++;
    } else if (!is_fp && !is_struct && gp2 < 8) {
      gp2++;
    } else if (is_struct && param->ty->size > 16 && gp2 < 8) {
      gp2++;  // indirect reference
    } else if (is_struct && param->ty->size <= 16 && gp2 + gp_needed <= 8) {
      gp2 += gp_needed;
    } else {
      // This param comes from caller's stack
      param->offset = stack_param_offset;
      stack_param_offset += align_to(param->ty->size < 8 ? 8 : param->ty->size, 8);
    }
  }

  // Assign negative offsets to all other locals
  int offset = 0;
    for (Obj *var = fn->locals; var; var = var->next) {
    if (var->offset > 0) continue; // already assigned (stack param)
    offset += var->ty->size;
    offset = align_to(offset, var->align);
    var->offset = -offset;
  }
  fn->stack_size = align_to(offset, 16);
}

//
// Emit data section for global variables
//

static void emit_data(Obj *prog) {
  // Track emitted names to avoid duplicates
  HashMap emitted = {};

  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function || !var->is_definition)
      continue;
    if (var->ty->size < 0)
      continue; // incomplete type, skip
    if (hashmap_get(&emitted, var->name))
      continue; // already emitted
    if (var->is_tentative && (var->ty->kind == TY_FUNC || (var->name[0] == '_' && var->name[1] == '_')))
      continue; // skip tentative system variables
    hashmap_put(&emitted, var->name, (void *)1);

    // Alias: emit .set directive
    if (var->alias_target) {
      if (!var->is_static)
        println(".globl _%s", var->name);
      println(".set _%s, _%s", var->name, var->alias_target);
      continue;
    }

    // Section
    if (var->init_data) {
      printlabel(".section __DATA,__data");
    } else {
      printlabel(".section __DATA,__bss");
    }

    // Alignment
    int align_power = 0;
    int a = var->align;
    while (a > 1) { align_power++; a >>= 1; }
    println(".p2align %d", align_power);

    // Visibility
    if (!var->is_static)
      println(".globl _%s", var->name);

    printlabel("_%s:", var->name);

    if (var->init_data) {
      // Emit initialized data (use init_data_size for flexible array members)
      int data_size = var->init_data_size ? var->init_data_size : var->ty->size;
      for (int i = 0; i < data_size; i++) {
        // Check for relocations at this offset
        Relocation *rel = NULL;
        for (Relocation *r = var->rel; r; r = r->next) {
          if (r->offset == i) {
            rel = r;
            break;
          }
        }

        if (rel) {
          // Emit a pointer relocation.
          // Computed goto labels (_cg_) already have the underscore prefix
          // and are non-local symbols that can be referenced cross-section.
          char *sym = *rel->label;
          if (!strncmp(sym, "_cg_", 4))
            println(".quad %s+%ld", sym, rel->addend);
          else
            println(".quad _%s+%ld", sym, rel->addend);
          i += 7; // skip 8 bytes
        } else {
          println(".byte %d", (unsigned char)var->init_data[i]);
        }
      }
    } else {
      // Uninitialized data
      println(".space %ld", var->ty->size);
    }
  }
}

//
// Emit text section for functions
//

static void emit_text(Obj *prog) {
  HashMap emitted_fns = {};
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_definition)
      continue;
    if (hashmap_get(&emitted_fns, fn->name))
      continue;
    // Emit non-static inline functions as weak definitions so that
    // the linker can deduplicate if the same inline appears in multiple TUs.
    // (This handles both gnu89-inline and C99 inline semantics for single-TU builds.)
    hashmap_put(&emitted_fns, fn->name, (void *)1);

    // Skip static inline functions that aren't used
    // (simplified — emit all for now)

    printlabel(".section __TEXT,__text");
    if (!fn->is_static) {
      if (fn->is_inline) {
        println(".weak_definition _%s", fn->name);
      }
      println(".globl _%s", fn->name);
    }
    println(".p2align 2");
    printlabel("_%s:", fn->name);

    current_fn = fn;
    // Assign offsets for all enclosing functions first, so that
    // chain variable accesses use correct offsets for outer locals.
    if (fn->is_nested) {
      for (Obj *enc = fn->enclosing_fn; enc; enc = enc->enclosing_fn) {
        if (enc->stack_size == 0 && enc->is_function && enc->is_definition)
          assign_lvar_offsets(enc);
      }
    }
    assign_lvar_offsets(fn);

    // Prologue
    // Save frame pointer and link register
    println("stp x29, x30, [sp, #-16]!");
    println("mov x29, sp");

    // Allocate stack space for locals
    if (fn->stack_size > 0) {
      if (fn->stack_size <= 4095)
        println("sub sp, sp, #%d", fn->stack_size);
      else {
        load_imm("x9", (uint64_t)fn->stack_size);
        println("sub sp, sp, x9");
      }

      // Check if function has VLA declarations (including structs with VLA members)
      bool has_vla = false;
      for (Obj *v = fn->locals; v; v = v->next)
        if (v->ty->kind == TY_VLA || v->ty->vla_size) { has_vla = true; break; }

      // Zero-initialize the stack frame when VLAs are present.
      // This ensures VLA saved_sp variables start at zero so the
      // first-time check (cbz) works correctly.
      if (has_vla) {
        int remaining = fn->stack_size;
        int off = 0;
        while (remaining >= 16) {
          println("stp xzr, xzr, [sp, #%d]", off);
          off += 16;
          remaining -= 16;
        }
        if (remaining >= 8) {
          println("str xzr, [sp, #%d]", off);
          off += 8;
          remaining -= 8;
        }
      }
    }

    // Save callee-saved registers if needed
    // (simplified — save x19-x28 if function is complex)

    // Store parameters from registers to their stack slots
    // Helper: compute address of local var in x9
    #define LOAD_PARAM_ADDR(off) do { \
      if ((off) < 0 && -(off) <= 4095) \
        println("sub x9, x29, #%d", -(off)); \
      else if ((off) < 0) { \
        load_imm("x9", (uint64_t)(-(off))); \
        println("sub x9, x29, x9"); \
      } else if ((off) <= 4095) \
        println("add x9, x29, #%d", (off)); \
      else { \
        load_imm("x9", (uint64_t)(off)); \
        println("add x9, x29, x9"); \
      } \
    } while(0)

    int gp = 0, fp = 0;
    for (Obj *param = fn->params; param; param = param->next) {
      // Skip stack-passed params (positive offset = on caller's stack)
      if (param->offset > 0) {
        if (is_flonum(param->ty)) fp++;
        else if ((param->ty->kind == TY_STRUCT || param->ty->kind == TY_UNION || param->ty->kind == TY_COMPLEX || param->ty->kind == TY_VECTOR) && param->ty->size > 16)
          gp += 1;  // indirect reference
        else if (param->ty->kind == TY_STRUCT || param->ty->kind == TY_UNION || param->ty->kind == TY_COMPLEX || param->ty->kind == TY_VECTOR)
          gp += (param->ty->size > 8) ? 2 : 1;
        else gp++;
        continue;
      }
      LOAD_PARAM_ADDR(param->offset);
      if (is_flonum(param->ty)) {
        if (fp < 8) {
          if (param->ty->kind == TY_FLOAT) {
            println("fcvt s0, %s", fpreg[fp]);
            println("str s0, [x9]");
          } else {
            println("str %s, [x9]", fpreg[fp]);
          }
          fp++;
        }
      } else if (param->ty->kind == TY_STRUCT || param->ty->kind == TY_UNION || param->ty->kind == TY_COMPLEX || param->ty->kind == TY_VECTOR) {
        if (param->ty->size > 16 && gp < 8) {
          // Large struct: passed by indirect reference (pointer in GP reg).
          // Copy from the pointer to local storage.
          println("mov x10, %s", argreg64[gp]);
          for (int j = 0; j + 7 < param->ty->size; j += 8) {
            println("ldr x11, [x10, #%d]", j);
            println("str x11, [x9, #%d]", j);
          }
          int rem2 = param->ty->size % 8;
          int base2 = param->ty->size - rem2;
          if (rem2 >= 4) {
            println("ldr w11, [x10, #%d]", base2);
            println("str w11, [x9, #%d]", base2);
            base2 += 4; rem2 -= 4;
          }
          if (rem2 >= 2) {
            println("ldrh w11, [x10, #%d]", base2);
            println("strh w11, [x9, #%d]", base2);
            base2 += 2; rem2 -= 2;
          }
          if (rem2 >= 1) {
            println("ldrb w11, [x10, #%d]", base2);
            println("strb w11, [x9, #%d]", base2);
          }
          gp++;
        } else if (param->ty->size == 1 && gp < 8) {
          println("strb %s, [x9]", argreg8[gp]);
          gp++;
        } else if (param->ty->size == 2 && gp < 8) {
          println("strh %s, [x9]", argreg8[gp]);
          gp++;
        } else if (param->ty->size == 3 && gp < 8) {
          println("strh %s, [x9]", argreg8[gp]);
          println("ubfx w10, %s, #16, #8", argreg8[gp]);
          println("strb w10, [x9, #2]");
          gp++;
        } else if (param->ty->size <= 4 && gp < 8) {
          println("str %s, [x9]", argreg8[gp]);
          gp++;
        } else if (param->ty->size == 5 && gp < 8) {
          println("str %s, [x9]", argreg8[gp]);
          println("ubfx x10, %s, #32, #8", argreg64[gp]);
          println("strb w10, [x9, #4]");
          gp++;
        } else if (param->ty->size == 6 && gp < 8) {
          println("str %s, [x9]", argreg8[gp]);
          println("ubfx x10, %s, #32, #16", argreg64[gp]);
          println("strh w10, [x9, #4]");
          gp++;
        } else if (param->ty->size == 7 && gp < 8) {
          println("str %s, [x9]", argreg8[gp]);
          println("ubfx x10, %s, #32, #16", argreg64[gp]);
          println("strh w10, [x9, #4]");
          println("ubfx x10, %s, #48, #8", argreg64[gp]);
          println("strb w10, [x9, #6]");
          gp++;
        } else if (param->ty->size <= 8 && gp < 8) {
          println("str %s, [x9]", argreg64[gp]);
          gp++;
        } else if (param->ty->size <= 16 && gp + 1 < 8) {
          println("str %s, [x9]", argreg64[gp]);
          // Only store the bytes that belong to the struct
          int remaining = param->ty->size - 8;
          if (remaining == 8)
            println("str %s, [x9, #8]", argreg64[gp + 1]);
          else if (remaining == 4)
            println("str %s, [x9, #8]", argreg8[gp + 1]);
          else {
            // Store byte-by-byte for odd sizes (rare)
            println("str %s, [x9, #8]", argreg8[gp + 1]);
          }
          gp += 2;
        } else if (gp < 8) {
          gp++;
        }
      } else {
        if (gp < 8) {
          if (param->ty->size == 1)
            println("strb %s, [x9]", argreg8[gp]);
          else if (param->ty->size == 2)
            println("strh %s, [x9]", argreg8[gp]);
          else if (param->ty->size == 4)
            println("str %s, [x9]", argreg8[gp]);
          else
            println("str %s, [x9]", argreg64[gp]);
          gp++;
        }
      }
    }
    #undef LOAD_PARAM_ADDR

    // Save variadic argument area pointer
    if (fn->is_variadic && fn->va_area) {
      // On Apple ARM64, variadic args are on the caller's stack.
      // After our prologue (stp x29,x30,[sp,#-16]!; mov x29,sp; sub sp,sp,#N),
      // the caller's variadic args are at x29+16.
      int off = fn->va_area->offset;
      if (off < 0 && -off <= 4095) {
        println("sub x9, x29, #%d", -off);
      } else if (off < 0) {
        load_imm("x9", (uint64_t)(-off));
        println("sub x9, x29, x9");
      } else if (off <= 4095) {
        println("add x9, x29, #%d", off);
      } else {
        load_imm("x9", (uint64_t)off);
        println("add x9, x29, x9");
      }
      println("add x10, x29, #16");
      println("str x10, [x9]");
    }

    // Initialize alloca bottom
    if (fn->alloca_bottom) {
      int off = fn->alloca_bottom->offset;
      println("mov x10, sp");
      if (off < 0 && -off <= 4095) {
        println("sub x9, x29, #%d", -off);
      } else if (off < 0) {
        load_imm("x9", (uint64_t)(-off));
        println("sub x9, x29, x9");
      } else if (off <= 4095) {
        println("add x9, x29, #%d", off);
      } else {
        load_imm("x9", (uint64_t)off);
        println("add x9, x29, x9");
      }
      println("str x10, [x9]");
    }

    // Set up non-local goto buffers for nested function longjmp targets.
    // Each buf stores [fp, sp, label_addr] so longjmp can restore the frame.
    for (NLGoto *nl = fn->nlgoto_targets; nl; nl = nl->next) {
      int off = nl->buf->offset;
      // Compute buf address in x9
      if (off < 0 && -off <= 4095) {
        println("sub x9, x29, #%d", -off);
      } else if (off < 0) {
        load_imm("x9", (uint64_t)(-off));
        println("sub x9, x29, x9");
      } else if (off <= 4095) {
        println("add x9, x29, #%d", off);
      } else {
        load_imm("x9", (uint64_t)off);
        println("add x9, x29, x9");
      }
      println("str x29, [x9]");         // buf[0] = fp
      println("mov x10, sp");
      println("str x10, [x9, #8]");     // buf[1] = sp
      println("adr x10, %s", nl->unique_label);
      println("str x10, [x9, #16]");    // buf[2] = label addr
    }

    // Generate function body
    gen_stmt(fn->body);

    // Check stack depth
    assert(depth == 0);

    // Epilogue
    printlabel(".L.return.%s:", fn->name);

    // C99 5.1.2.2.3: implicit return 0 from main()
    if (!strcmp(fn->name, "main"))
      println("mov x0, #0");

    // Restore stack
    println("mov sp, x29");
    println("ldp x29, x30, [sp], #16");
    println("ret");
  }
}

//
// Main codegen entry point
//

void codegen(Obj *prog, FILE *out) {
  output_file = out;

  // Emit assembly
  emit_data(prog);
  emit_text(prog);

  // Ensure the file doesn't trigger NX protection issues
  printlabel(".section __DATA,__data");
}
