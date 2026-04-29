/* ARM64 stub for asm/static_call.h (ncc scan only) */
#ifndef _ASM_STATIC_CALL_H
#define _ASM_STATIC_CALL_H

#define CALL_INSN_SIZE 4  /* ARM64: BL instruction is 4 bytes */

#define ARCH_DEFINE_STATIC_CALL_TRAMP(name, func)
#define ARCH_DEFINE_STATIC_CALL_NULL_TRAMP(name)
#define ARCH_DEFINE_STATIC_CALL_RET0_TRAMP(name)

#endif /* _ASM_STATIC_CALL_H */
