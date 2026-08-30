#ifndef JIT_ARMV6M_RUNTIME_HOST_H_
#define JIT_ARMV6M_RUNTIME_HOST_H_

#ifndef __ASSEMBLER__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t value;
    uint32_t trapped;
} ProgramResult;

#define ARMV6M_EXCEPTION_FRAME_BYTES 32

ProgramResult enterProgramOnStack(
    uint32_t *args,
    uint32_t argCount,
    const uint8_t *programBytes,
    uint32_t programSize,
    uint32_t codeArenaSize,
    uint32_t stackLimit,
    uint32_t interruptReserve);

ProgramResult enterProgramSplit(
    uint32_t *args,
    uint32_t argCount,
    const uint8_t *programBytes,
    uint32_t programSize,
    uint32_t codeArenaBase,
    uint32_t codeArenaSize,
    uint32_t stackLimit,
    uint32_t interruptReserve);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */

#define DISPATCH_SENTINEL_OFFSET 16      /* dispatchBase - this = &slots[0] (the sentinel) */

#define CALL_RECORD_BOOT 0xffff          /* == packRecord(-1, 0) — asserted in dispatch_abi.cpp */

#define LANDING_SUCCESS 0u        /* value = the entry procedure's own result */
#define LANDING_TRAP 1u           /* value = the bytecode TRAP's own code, at any call depth */
#define LANDING_RESOURCE_ERROR 2u /* value = one of the RESOURCE_* codes below — nothing ran, or ran to completion */

#define RESOURCE_ERROR_SIGNATURE 0x5245u /* == (value >> 16) for every code below */
#define RESOURCE_ERROR_CLASS(v) (((v) >> 12) & 0xfu)
#define RESOURCE_ERROR_CLASS_PROGRAM 1u
#define RESOURCE_ERROR_CLASS_EXHAUSTED 2u
#define RESOURCE_ERROR_CLASS_LIMIT 3u

#define RESOURCE_PROGRAM_NO_PROCS 0x52451100u          /* proc_count is zero */
#define RESOURCE_PROGRAM_BODY_UNTERMINATED 0x52451200u /* a body ran off the blob with a block still open */
#define RESOURCE_PROGRAM_CALLEE_RANGE 0x52451300u      /* CALL names a procedure index the program doesn't have */
#define RESOURCE_PROGRAM_ENTRY_ARG_COUNT 0x52451500u /* argCount != the entry procedure's own declared arg_count */
#define RESOURCE_PROGRAM_ENTRY_DEPTH 0x52451600u     /* entry procedure's out-of-window args over the envelope's own total_depth */
#define RESOURCE_PROGRAM_EXT_UNKNOWN 0x52451700u
#define RESOURCE_PROGRAM_EXT_UNSUPPORTED 0x52451800u
#define RESOURCE_PROGRAM_RESERVED_OPCODE 0x52451a00u

#define RESOURCE_EXHAUSTED_ARENA 0x52452100u            /* code arena full with nothing left to evict */
#define RESOURCE_EXHAUSTED_STACK_BUDGET 0x52452200u     /* the up-front whole-excursion stack check failed; nothing was touched */
#define RESOURCE_EXHAUSTED_TRANSLATOR_STACK 0x52452300u /* the translator's own recursion reached the live stack floor */
#define RESOURCE_EXHAUSTED_SCAN_STACK 0x52452400u       /* ditto, in the pre-pass that builds the directory */

#define RESOURCE_LIMIT_WINDOW_RECLAIM 0x52453100u /* frame reclaim past ADD sp,#imm's reach — TOS depth over 131 */
#define RESOURCE_LIMIT_SPILL_OFFSET 0x52453200u   /* SP-relative spill slot past LDR/STR [sp,#imm]'s 1020 bytes */
#define RESOURCE_LIMIT_BRANCH_RANGE 0x52453300u   /* a forward fixup resolved beyond Bcc/B reach */
#define RESOURCE_LIMIT_LOOP_BACK_EDGE 0x52453400u /* a LOOP body outgrew B's own backward reach */
#define RESOURCE_LIMIT_ARG_COUNT 0x52453500u      /* arg_count over ProcSlot's own field width */
#define RESOURCE_LIMIT_PROC_COUNT 0x52453700u     /* proc_count over the call record's own procIdx field */
#define RESOURCE_LIMIT_RESUME_OFFSET 0x52453800u  /* resume offset over the call record's own field */
#define RESOURCE_LIMIT_BODY_BYTES 0x52453600u /* body size over ProcSlot's own field width */

#endif /* JIT_ARMV6M_RUNTIME_HOST_H_ */
