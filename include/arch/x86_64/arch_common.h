#ifndef _ARCH_X86_64_H
#define _ARCH_X86_64_H

/** Intel endbr64 instruction optcode.  */
#define INSN_ENDBR64 0xf3, 0x0f, 0x1e, 0xfa

/** Offset of TLS pointer.  */
#define TLS_DTV_OFFSET 0

/** Struct used to store the registers in memory.  */
typedef struct user_regs_struct registers_t;

/** Register in which the function stores the return value.  */
#define FUNCTION_RETURN_REG(reg) ((reg).rax)

/** Register which acts as a program counter.  */
#define PROGRAM_COUNTER_REG(reg) ((reg).rip)

/** Register which acts as top of stack.  */
#define STACK_TOP_REG(reg)       ((reg).rsp)

/* Program load bias, which can be recovered by running `ld --verbose`.  */
#define EXECUTABLE_START      0x400000UL

/**
 * Number of bytes that the kernel subtracts from the program counter,
 * when an ongoing syscall gets interrupted and must be restarted.
 */
#define RESTART_SYSCALL_SIZE  2

#endif
