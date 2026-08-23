/* Public interface into the runtime's enterProgram family. Shared between
 * runtime_host.cpp and generated test programs so the FlashProc layout
 * can't drift between the two sides of that boundary. */

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
    const uint16_t *bytes; /* this procedure's own [stub][body] blob, in flash */
    uint32_t size;          /* its byte length */
} FlashProc;

typedef struct
{
    uint32_t value;   /* the entry procedure's own result, or (if trapped) a trap/error code */
    uint32_t trapped; /* 0: normal return. nonzero: RESOURCE_ERROR or a propagated bytecode TRAP */
} ProgramResult;

/* Sets up the dispatch table, then calls enterDispatch (runtime.S), which
 * dispatches into the entry procedure (index 0) and returns normally once
 * that either completes or bails out. callHelper/returnHelper/enterDispatch
 * are part of the fixed runtime, not parameters — this just extern-declares
 * them by name.
 *
 * Call/return records travel in lr, on the same operand stack (sp) window
 * spills already use — there is no separate control stack. */
ProgramResult enterProgram(
    uint32_t argIn,
    uint32_t arenaSize,
    const FlashProc *procs,
    uint32_t procCount);

/* One ARMv6-M exception frame (R0-R3,R12,LR,return-address,xPSR — 32
 * bytes). A convenience value for interruptReserve below, not the only
 * legal one — a caller with its own RTOS/ISR requirements beyond the bare
 * architectural minimum should pass whatever its own platform needs
 * instead. */
#define ARMV6M_EXCEPTION_FRAME_BYTES 32

/* enterProgram, but the current C stack is the whole work area: Runtime,
 * its dispatch table, the operand stack, and the compiled-code arena (sized
 * to codeArenaSize) all come out of it — not stacked one after the other.
 * The arena anchors at stackLimit and grows up from there; Runtime/the
 * operand stack grow down from wherever sp already is, converging toward
 * each other from opposite ends of the same checked range. This leaves room
 * for the translator to temporarily encroach into whatever part of the
 * arena's own reservation is still unused.
 *
 * Safe only because it's checked first: operandStackBytes (the program's
 * own worst-case TOS depth in bytes) and maxCallDepth (its worst-case live
 * call-chain length) are static, whole-program properties the caller is
 * expected to have already computed. stackLimit is the lowest address this
 * excursion must never reach or pass, and interruptReserve is how much
 * headroom to hold back for exception entry. On failure, the returned
 * ProgramResult reports RESOURCE_ERROR without ever touching any of that
 * memory or calling into enterDispatch at all. */
ProgramResult enterProgramOnStack(
    uint32_t argIn,
    const FlashProc *procs,
    uint32_t procCount,
    uint32_t codeArenaSize,
    uint32_t operandStackBytes,
    uint32_t maxCallDepth,
    uint32_t stackLimit,
    uint32_t interruptReserve);

/* enterProgram, but for hardware where the compiled-code arena wants to
 * live somewhere other than plain stack-adjacent RAM (a distinct SRAM bank,
 * CCM, whatever a given target's bus matrix makes worth using) —
 * codeArenaBase/codeArenaSize describe that caller-owned region directly.
 * Runtime, its dispatch table, and the operand stack still live on the
 * current C stack regardless. Same stack-limit checking as
 * enterProgramOnStack, except codeArenaSize never enters that check at all,
 * since that memory isn't on this stack to begin with. */
ProgramResult enterProgramSplit(
    uint32_t argIn,
    const FlashProc *procs,
    uint32_t procCount,
    uint32_t codeArenaBase,
    uint32_t codeArenaSize,
    uint32_t operandStackBytes,
    uint32_t maxCallDepth,
    uint32_t stackLimit,
    uint32_t interruptReserve);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */

/* runtime.S's own layout constants for reaching the Runtime struct from a
 * bare address — enterDispatch gets the runtime pointer directly (r9), but
 * still needs its dispatch table's own base address (r8) and the sentinel
 * slot one DispatchEntry before it; the static_asserts in
 * runtime_internal.h tie these back to the real struct, so any layout drift
 * fails the build instead of corrupting memory silently. */
#define RUNTIME_DISPATCH_TABLE_OFFSET 40 /* &runtime + this = dispatchBase (== &dispatchTable[1]) */
#define DISPATCH_SENTINEL_OFFSET 8       /* dispatchBase - this = &dispatchTable[0] (the sentinel) */
