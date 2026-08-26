/* Public interface into the runtime's enterProgram family
 * (enter_program.cpp): the two entry points and the ProgramResult they
 * return. ProcSlot/Runtime themselves are not declared here — they're
 * internal (runtime_internal.h) — this header's own job is staying safe
 * for runtime.S to #include under __ASSEMBLER__ (see the two #defines at
 * the bottom), so it carries nothing beyond what a plain-C caller and the
 * assembler both need. */

/* runtime.S includes this too (for the two #defines at the bottom),
 * preprocessed but never compiled as C — __ASSEMBLER__ is predefined by GCC
 * for that case, so the struct/function declarations below stay hidden
 * from it. */
#ifndef __ASSEMBLER__

#include <stdint.h>

/* extern "C" so a plain-C host application can link this JIT in just as
 * easily as a C++ one. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t value;   /* the entry procedure's own result, or (if trapped) RESOURCE_ERROR_CODE */
    uint32_t trapped; /* 0: normal return — including a bytecode TRAP, which this slice has no
                        * real error-reporting model for and sentinel-encodes into value instead
                        * (translate_proc.cpp's own high-bit-set convention). nonzero: RESOURCE_ERROR. */
} ProgramResult;

/* programBytes/programSize is one whole serialized jit-armv6m program: the
 * jit-armv6m-specific envelope (max_call_depth:LEB128 total_depth:LEB128 —
 * packages/machine/src/bytecode.ts's encodeJitProgram) prepended to an
 * ordinary isa-core.md §5.5 program (proc_count:LEB128, then each
 * procedure's own arg_count:LEB128 immediately followed by its own body).
 * Both whole-program stats and proc_count come out of that envelope, not a
 * caller-supplied parameter — nothing else needs supplying separately, and
 * nothing here can drift out of sync with what the bytes themselves
 * actually contain. programSize bounds the walk against overflow, the same
 * asserted-not-recovered convention as everywhere else this translator
 * decodes wire bytes (decode_instr.h's own).
 *
 * Both variants below set up the dispatch table (and, from the walk above,
 * every slot's own static half — runtime_internal.h's ProcSlot), then call
 * enterDispatch (runtime.S), which dispatches into the entry procedure
 * (index 0) and returns normally once that either completes or bails out.
 * callHelper/returnHelper/enterDispatch are part of the fixed runtime, not
 * parameters — this just extern-declares them by name.
 *
 * Call/return records travel in lr, on the same operand stack (sp) window
 * spills already use — there is no separate control stack.
 *
 * Both are checked, up front, against the program's own worst-case TOS
 * depth and call-chain length (the envelope above) — static, whole-program
 * properties `validateProgram` already computed before this was ever
 * encoded. stackLimit is the lowest address the excursion must never reach
 * or pass, and interruptReserve is how much headroom to hold back for
 * exception entry. On failure, the returned ProgramResult reports
 * RESOURCE_ERROR without ever touching that memory or calling into
 * enterDispatch at all. There is no arena-less "just give me somewhere to
 * put the code" variant: a caller that wants a plain global arena declares
 * one itself (one line, sized to what it actually needs) and passes it to
 * enterProgramSplit below. */

/* One ARMv6-M exception frame (R0-R3,R12,LR,return-address,xPSR — 32
 * bytes). A convenience value for interruptReserve below, not the only
 * legal one — a caller with its own RTOS/ISR requirements beyond the bare
 * architectural minimum should pass whatever its own platform needs
 * instead. */
#define ARMV6M_EXCEPTION_FRAME_BYTES 32

/* The current C stack is the whole work area: Runtime, its dispatch table,
 * the operand stack, and the compiled-code arena (sized to codeArenaSize)
 * all come out of it — not stacked one after the other. The arena anchors
 * at stackLimit and grows up from there; Runtime/the operand stack grow
 * down from wherever sp already is, converging toward each other from
 * opposite ends of the same checked range. This leaves room for the
 * translator to temporarily encroach into whatever part of the arena's own
 * reservation is still unused. */
ProgramResult enterProgramOnStack(
    uint32_t argIn,
    const uint8_t *programBytes,
    uint32_t programSize,
    uint32_t codeArenaSize,
    uint32_t stackLimit,
    uint32_t interruptReserve);

/* For hardware where the compiled-code arena wants to live somewhere other
 * than plain stack-adjacent RAM (a distinct SRAM bank, CCM, whatever a
 * given target's bus matrix makes worth using, or just a plain static
 * array declared by the caller) — codeArenaBase/codeArenaSize describe
 * that caller-owned region directly. Runtime, its dispatch table, and the
 * operand stack still live on the current C stack regardless. Same
 * stack-limit checking as enterProgramOnStack, except codeArenaSize never
 * enters that check at all, since that memory isn't on this stack to begin
 * with. */
ProgramResult enterProgramSplit(
    uint32_t argIn,
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

/* runtime.S's own layout constants for reaching the Runtime struct from a
 * bare address — enterDispatch gets the runtime pointer directly (r9), but
 * still needs its dispatch table's own base address (r8) and the sentinel
 * slot one ProcSlot before it.
 *
 * These two #defines have to live in *this* header, not next to Runtime/
 * ProcSlot themselves (runtime_internal.h), because runtime.S #includes
 * this file under __ASSEMBLER__ and Runtime/ProcSlot are C++-only types a
 * bare preprocessor pass can't see — hence the split the two
 * static_asserts at the bottom of runtime_internal.h close: those check
 * the numbers *here* against Runtime's real layout *there*, so any drift
 * between the two files fails the build instead of corrupting memory
 * silently. Change a number on either side and the other file's own
 * assert is what catches the mismatch. */
#define RUNTIME_DISPATCH_TABLE_OFFSET 40 /* &runtime + this = dispatchBase (== &slots[1]) */
#define DISPATCH_SENTINEL_OFFSET 16      /* dispatchBase - this = &slots[0] (the sentinel) */
