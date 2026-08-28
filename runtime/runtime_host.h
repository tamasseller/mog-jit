/* Public interface into the runtime's enterProgram family
 * (enter_program.cpp): the two entry points and the ProgramResult they
 * return. ProcSlot/Runtime themselves are not declared here — they're
 * internal (runtime_internal.h) — this header's own job is staying safe
 * for runtime.S to #include under __ASSEMBLER__ (see the two #defines at
 * the bottom), so it carries nothing beyond what a plain-C caller and the
 * assembler both need.
 *
 * Include-guarded like any other header: it was previously only
 * __ASSEMBLER__-guarded, which was enough while exactly one translation
 * unit included it, and stopped being enough the moment a second one did
 * (ProgramResult would be redeclared). Safe under __ASSEMBLER__ too. */
#ifndef JIT_ARMV6M_RUNTIME_HOST_H_
#define JIT_ARMV6M_RUNTIME_HOST_H_

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
    /* What `value` means is decided entirely by `trapped` — see the
     * LANDING_* tags at the bottom of this header. Nothing is encoded in
     * `value`'s own bits: a program may return any uint32_t, and a trap
     * code may be any uint32_t, without either aliasing the other. */
    uint32_t value;
    uint32_t trapped;
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

/* ProgramResult::trapped — how the excursion ended, and therefore what
 * `value` holds. These are the tags enterDispatch's own sentinel landing
 * receives in r2 (runtime.S's .Lresume), so they live in this header
 * rather than runtime_internal.h: runtime.S #includes this file under
 * __ASSEMBLER__ and needs LANDING_TRAP by name.
 *
 * Every one of the three arrives at the same landing, and the three
 * routes to it are the whole story of how a compiled excursion can end:
 * an ordinary RETURN from the entry procedure (returnHelperTail resolving
 * the boot record's own procIdx of -1, whose "resume offset" field is
 * zero and so doubles as LANDING_SUCCESS), a bytecode TRAP at any call
 * depth (trapHelper, helper slot 8), or the translator running out of
 * arena (runtimeBail, from C++). */
#define LANDING_SUCCESS 0u        /* value = the entry procedure's own result */
#define LANDING_TRAP 1u           /* value = the bytecode TRAP's own code, at any call depth */
#define LANDING_RESOURCE_ERROR 2u /* value = RESOURCE_ERROR_CODE — no arena/stack room, nothing ran or ran to completion */

#endif /* JIT_ARMV6M_RUNTIME_HOST_H_ */
