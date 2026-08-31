#ifndef JIT_ARMV6M_RUNTIME_RESOURCE_CODES_H_
#define JIT_ARMV6M_RUNTIME_RESOURCE_CODES_H_

/* Every way the JIT refuses a program, as one flat vocabulary. A code carries
 * the 0x5245 signature in its top half and a class nibble below it, so a
 * caller can tell one of these from a program's own trap value without a
 * lookup table; the static_assert at the bottom is what keeps that true. */

#define RESOURCE_ERROR_SIGNATURE 0x5245u /* == (value >> 16) for every code below */
#define RESOURCE_ERROR_CLASS(v) (((v) >> 12) & 0xfu)
#define RESOURCE_ERROR_CLASS_PROGRAM 1u
#define RESOURCE_ERROR_CLASS_EXHAUSTED 2u
#define RESOURCE_ERROR_CLASS_LIMIT 3u

#define RESOURCE_PROGRAM_NO_PROCS 0x52451100u          /* proc_count is zero */
#define RESOURCE_PROGRAM_BODY_UNTERMINATED 0x52451200u /* a body ran off the blob with a block still open */
#define RESOURCE_PROGRAM_CALLEE_RANGE 0x52451300u      /* CALL names a procedure index the program doesn't have */
#define RESOURCE_PROGRAM_FRAME 0x52451400u             /* truncated, corrupt, or built against another contract version */
#define RESOURCE_PROGRAM_ENTRY_ARG_COUNT 0x52451500u /* argCount != the entry procedure's own declared arg_count */
#define RESOURCE_PROGRAM_ENTRY_DEPTH 0x52451600u     /* entry procedure's out-of-window args over the envelope's own total_depth */
#define RESOURCE_PROGRAM_EXT_UNKNOWN 0x52451700u
#define RESOURCE_PROGRAM_EXT_UNSUPPORTED 0x52451800u

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

#ifdef __cplusplus

#include <stdint.h>

namespace
{
constexpr uint32_t RESOURCE_CODES[] = {
    RESOURCE_PROGRAM_NO_PROCS, RESOURCE_PROGRAM_BODY_UNTERMINATED,
    RESOURCE_PROGRAM_CALLEE_RANGE, RESOURCE_PROGRAM_FRAME,
    RESOURCE_PROGRAM_ENTRY_ARG_COUNT,
    RESOURCE_PROGRAM_ENTRY_DEPTH, RESOURCE_PROGRAM_EXT_UNKNOWN,
    RESOURCE_PROGRAM_EXT_UNSUPPORTED,
    RESOURCE_EXHAUSTED_ARENA, RESOURCE_EXHAUSTED_STACK_BUDGET,
    RESOURCE_EXHAUSTED_TRANSLATOR_STACK, RESOURCE_EXHAUSTED_SCAN_STACK,
    RESOURCE_LIMIT_WINDOW_RECLAIM, RESOURCE_LIMIT_SPILL_OFFSET,
    RESOURCE_LIMIT_BRANCH_RANGE, RESOURCE_LIMIT_LOOP_BACK_EDGE,
    RESOURCE_LIMIT_ARG_COUNT, RESOURCE_LIMIT_BODY_BYTES,
    RESOURCE_LIMIT_PROC_COUNT, RESOURCE_LIMIT_RESUME_OFFSET,
};

constexpr bool resourceCodesDistinct()
{
    for(unsigned i = 0; i < sizeof(RESOURCE_CODES) / sizeof(RESOURCE_CODES[0]); i++)
    {
        for(unsigned j = i + 1; j < sizeof(RESOURCE_CODES) / sizeof(RESOURCE_CODES[0]); j++)
        {
            if(RESOURCE_CODES[i] == RESOURCE_CODES[j])
            {
                return false;
            }
        }
        if((RESOURCE_CODES[i] >> 16) != RESOURCE_ERROR_SIGNATURE
            || RESOURCE_ERROR_CLASS(RESOURCE_CODES[i]) == 0
            || (RESOURCE_CODES[i] & 0xffu) != 0)
        {
            return false;
        }
    }
    return true;
}
} // namespace

static_assert(resourceCodesDistinct(),
    "RESOURCE_* codes must be distinct, carry the 0x5245 signature and a class nibble, and leave the low byte zero");

#endif /* __cplusplus */

#endif /* JIT_ARMV6M_RUNTIME_RESOURCE_CODES_H_ */
