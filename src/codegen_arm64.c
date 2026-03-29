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

int align_to(int n, int align) {
  return (n + align - 1) / align * align;
}

#define println(...)  do { fprintf(output_file, "\t"); fprintf(output_file, __VA_ARGS__); fprintf(output_file, "\n"); } while(0)
#define printlabel(...) do { fprintf(output_file, __VA_ARGS__); fprintf(output_file, "\n"); } while(0)

static void gen_expr(Node *node);
static void gen_stmt(Node *node);
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
  if (is_flonum(node->ty)) {
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

// Load value from memory according to type
static void load(Type *ty) {
  switch (ty->kind) {
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION:
  case TY_FUNC:
  case TY_VLA:
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
  case TY_UNION: {
    // Copy struct from x0 (src addr) to x1 (dst addr)
    for (int i = 0; i < ty->size; i++) {
      println("ldrb w2, [x0, #%d]", i);
      println("strb w2, [x1, #%d]", i);
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
    for (int i = 0; i < ty->size; i++) {
      println("ldrb w2, [x0, #%d]", i);
      println("strb w2, [%s, #%d]", addr_reg, i);
    }
    return;
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
          println("mov x0, #%d", off);
          println("sub x0, x29, x0");
        }
      } else {
        if (node->var->offset <= 4095) {
          println("add x0, x29, #%d", node->var->offset);
        } else {
          println("mov x0, #%d", node->var->offset);
          println("add x0, x29, x0");
        }
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
    println("add x0, x0, #%d", node->member->offset);
    return;

  case ND_FUNCALL:
    gen_expr(node);
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

  default:
    error_tok(node->tok, "not an lvalue");
  }
}

// Cast value in x0/d0 to a target type
static void cast(Type *from, Type *to) {
  if (to->kind == TY_VOID)
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
    // d0 → x0
    if (to->is_unsigned) {
      println("fcvtzu x0, d0");
    } else {
      println("fcvtzs x0, d0");
    }
    // Truncate to appropriate size
    if (to->size <= 4)
      println("sxtw x0, w0");
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
    bool is_struct = (aty->kind == TY_STRUCT || aty->kind == TY_UNION);
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
      bool is_struct_arg = aty && (aty->kind == TY_STRUCT || aty->kind == TY_UNION);
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
      bool is_struct = aty && (aty->kind == TY_STRUCT || aty->kind == TY_UNION);

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
    bool is_struct = aty && (aty->kind == TY_STRUCT || aty->kind == TY_UNION);

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
      if (node->member->ty->is_unsigned) {
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

      println("mov x0, x10"); // return the assigned value
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
      println("mov x1, #%d", -off);
      println("sub x1, x29, x1");
    } else {
      println("add x1, x29, #%d", off);
    }

    println("mov x2, #0");
    for (int i = 0; i < sz; i += 8) {
      if (i + 8 <= sz)
        println("str x2, [x1, #%d]", i);
      else {
        for (int j = i; j < sz; j++)
          println("strb w2, [x1, #%d]", j);
        break;
      }
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

  case ND_LABEL_VAL: {
    println("adr x0, %s", node->unique_label);
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

  default:
    break;
  }

  // Binary operations
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

static void gen_stmt(Node *node) {
  add_type(node);
  switch (node->kind) {
  case ND_IF: {
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
        println("mov x9, #%ld", n->begin);
        println("cmp x0, x9");
        println("b.lt .L.next.%s", n->label);
        println("mov x9, #%ld", n->end);
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
    println("b %s", node->unique_label);
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

  case ND_ASM:
    println("%s", node->asm_str);
    return;

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
    bool is_struct = (param->ty->kind == TY_STRUCT || param->ty->kind == TY_UNION);

    if (is_fp && fp2 < 8) {
      fp2++;
    } else if (!is_fp && gp2 < 8) {
      if (is_struct && param->ty->size > 16) gp2 += 1;  // indirect reference
      else if (is_struct) gp2 += (param->ty->size > 8) ? 2 : 1;
      else gp2++;
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
    if (var->ty->size < 0 || var->ty->size == 0)
      continue; // incomplete type, skip
    if (hashmap_get(&emitted, var->name))
      continue; // already emitted
    if (var->is_tentative && (var->ty->kind == TY_FUNC || (var->name[0] == '_' && var->name[1] == '_')))
      continue; // skip tentative system variables
    hashmap_put(&emitted, var->name, (void *)1);

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
      // Emit initialized data
      for (int i = 0; i < var->ty->size; i++) {
        // Check for relocations at this offset
        Relocation *rel = NULL;
        for (Relocation *r = var->rel; r; r = r->next) {
          if (r->offset == i) {
            rel = r;
            break;
          }
        }

        if (rel) {
          // Emit a pointer relocation
          println(".quad _%s+%ld", *rel->label, rel->addend);
          i += 7; // skip 8 bytes
        } else {
          println(".byte %d", (unsigned char)var->init_data[i]);
        }
      }
    } else {
      // Uninitialized data
      println(".space %d", var->ty->size);
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
    // Skip inline functions from system headers (names starting with __)
    // Skip inline functions from headers — they get emitted in every TU
    if (fn->is_inline && !fn->is_static)
      continue;
    hashmap_put(&emitted_fns, fn->name, (void *)1);

    // Skip static inline functions that aren't used
    // (simplified — emit all for now)

    printlabel(".section __TEXT,__text");
    if (!fn->is_static)
      println(".globl _%s", fn->name);
    println(".p2align 2");
    printlabel("_%s:", fn->name);

    current_fn = fn;
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
        println("mov x9, #%d", fn->stack_size);
        println("sub sp, sp, x9");
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
        println("mov x9, #%d", -(off)); \
        println("sub x9, x29, x9"); \
      } else if ((off) <= 4095) \
        println("add x9, x29, #%d", (off)); \
      else { \
        println("mov x9, #%d", (off)); \
        println("add x9, x29, x9"); \
      } \
    } while(0)

    int gp = 0, fp = 0;
    for (Obj *param = fn->params; param; param = param->next) {
      // Skip stack-passed params (positive offset = on caller's stack)
      if (param->offset > 0) {
        if (is_flonum(param->ty)) fp++;
        else if ((param->ty->kind == TY_STRUCT || param->ty->kind == TY_UNION) && param->ty->size > 16)
          gp += 1;  // indirect reference
        else if (param->ty->kind == TY_STRUCT || param->ty->kind == TY_UNION)
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
      } else if (param->ty->kind == TY_STRUCT || param->ty->kind == TY_UNION) {
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
      } else {
        println("mov x9, #%d", -off);
        println("sub x9, x29, x9");
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
        println("mov x9, #%d", -off);
        println("sub x9, x29, x9");
      } else {
        println("add x9, x29, #%d", off);
      }
      println("str x10, [x9]");
    }

    // Generate function body
    gen_stmt(fn->body);

    // Check stack depth
    assert(depth == 0);

    // Epilogue
    printlabel(".L.return.%s:", fn->name);

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
